#pragma once
#include <runtime/world/minefield.h>

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
#include <godot_cpp/variant/typed_array.hpp>
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

#include <runtime/controls/controls.h> // WeaponCategory (the bound enum's value authority)
#include <runtime/mission/event_runtime.h>
#include <runtime/mission/promote.h>
#include <runtime/hud/end_round_overlay.h> // EndRoundOverlayInput (the end-round ladder feed)
#include <runtime/hud/hud_frame.h> // HudVehiclePanelState / HudLfpZone (the panel feed seams)
#include <runtime/world/deploy_screen_feed.h> // DeployZoneRow (the DEATH screen's zone feed)
#include <runtime/hud/hud_minimap.h>
#include <runtime/hud/hud_minimap_feed.h> // the marker feed layout the snapshot carries
#include <runtime/world/deploy_screen_feed.h> // kDeployRefreshTicks (the death deploy screen cadence)
#include <runtime/world/present_drains.h> // the per-tick presentation drain rows (ADR 0043 d10)
#include <runtime/world/friendly_tags.h> // FriendlyTagSource (the D-HUD-20 gather)
#include <runtime/world/vehicle_attach.h> // AttachLabel (the attach-label scan)
#include <runtime/world/destruction.h> // DestructionEvents (the destruction drain)
#include <runtime/world/terrain_scorch_events.h> // TerrainScorchEvent (the scorch drain)
#include <runtime/world/script_voice.h>
#include <runtime/world/sound_emitter_mailbox.h> // SoundEmitterEvent (the emitter drain)
#include <runtime/world/fire_sound.h> // ReadyFireSound (the fire-sound drain)
#include <formats/playersav/weapon_sav.h> // weapon.sav: the per-side profile class + kit pages
#include <runtime/terrain_query/terrain_field_store.h>
#include <runtime/wac/wac_system.h>

namespace godot {

class Weather;
class RtxtStringFile; // the gametext table the end-round / deploy feeds resolve through
class EntityCard;     // the typed per-entity debug card (world::inspect, ADR 0042 d5)
class EntityRow;      // one typed entity-directory row
class FeedRow;        // one typed message-feed row (hud::FeedRow, ADR 0040 B3)
class WeaponDef;      // one weapon.def row as a typed record (object/weapon_def.h)
class CharacterJoinProfile; // the two-side character selection (object/character_join_profile.h)
class FpViewmodelSpec;      // the first-person submit spec (simulation/fp_viewmodel_spec.h)
class HostSessionOptions;   // the hosted-session request (network/host_session_options.h)
class PlayerLocalView;      // the local view-state snapshot (simulation/player_local_view.h)
class PlayerAimOverlay;     // the local per-segment aim overlay (simulation/player_aim_overlay.h)
class PlayerWeaponView;     // the local weapon FSM view (simulation/player_weapon_view.h)
class PlayerWeaponEvent;    // one ordered weapon presentation event (simulation/player_weapon_event.h)
class ScarDrawList;         // one frame's impact-scar draw list (world/scar_draw_list.h)
class WeaponKitEntry;       // one loadout tuple (simulation/weapon_kit_entry.h)
class WeaponProfileSummary; // the weapon.sav slot-0 summary (simulation/weapon_profile_summary.h)
class EnvironmentSnapshot;  // the F3 Environment record as a typed read (simulation/environment_snapshot.h)
class PlayerInventory;      // the local inventory snapshot (simulation/player_inventory.h)
class EndRoundState;  // the typed end-of-round session facts (simulation_end_round.cpp)
// The small per-frame HUD view records (simulation/hud_view_records.h).
class WaypointHudView;
class HudMapGridOrigin;
class VehiclePanelView;
class ScoreFeedback;
class ScoreboardHeader;
class EndRoundOverlay;
class EndRoundStatistics;
class DeployStatus;
class DestructionDrain;  // the destruction drain record (simulation/destruction_events.h)
class HitboxDebugReport; // the hitbox oracle payload (simulation/hitbox_debug_report.h)
class DebugPickCard;     // the entity picker's card (simulation/debug_pick_card.h)
}

#include "wac/wac_program.h"
#include <formats/def/def.h> // the retained weapon.def parse (S6b)
#include <runtime/simassets/adm_clip_index.h> // the equipped rig's clip lengths (S6b)
#include <runtime/world/player_loadout.h> // the moved loadout cluster (S7b, ADR 0028)
#include <runtime/world/player_weapon.h> // the moved equipped-weapon cluster (S7a, ADR 0028)
#include <runtime/world/present_rows.h> // the engine-owned PF_* present-row layout (ADR 0031)
#include <runtime/simassets/collision_resolve.h> // the collision/occlusion resolution sweep (ADR 0031)
#include <runtime/simassets/sim_pose_provider.h> // the engine-side pose provider (S3, ADR 0028)
#include <runtime/simassets/sim_model_cache.h> // the sim's own .3di source (ADR 0028)
#include <runtime/simassets/mounted_pose.h> // reusable PANM part matrices for mounted attachments
#include <runtime/inmatch/session.h>
#include <runtime/inmatch/present_rows.h> // PoolPresentLifecycleMap (the host present path's respawn mirror)
#include <runtime/world/ai.h>
#include <runtime/world/inspect.h> // the typed entity inspection API (ADR 0042 d5)
#include <runtime/world/tick_accumulator.h>
#include <runtime/world/collision.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/player_input.h>
#include <runtime/world/player_look.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_view.h>
#include <runtime/world/vehicle_attach.h> // the attach-command ids + the seat mirror
#include <runtime/world/round_sim.h> // the hit-zone damage tables (re-exported statics)
#include <runtime/world/spawn_select.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>
#include <formats/score/score.h> // the retained score.ini parse (assets_.score_config)

#include "mission/mission_data.h"
#include <runtime/mission/mission_kernel.h>
#include <runtime/renderer/precipitation_frame.h>
#include <runtime/devtools/environment_snapshot.h> // the ONE mission boot + state + no-net tick (ADR 0042 d3)
#include <runtime/devtools/physics_snapshot.h>
#include <runtime/devtools/rays_snapshot.h>
#include <runtime/simassets/adm_root_motion.h> // the engine-side IRootMotionSource (ADR 0028)

#include <runtime/inmatch/host_role.h>                // HostRole: the listen/dedicated host's state + frame (ADR 0043 d3)
#include <runtime/inmatch/joiner_role.h>              // JoinerRole: the joiner's runtime, bridge and frame
#include <runtime/inmatch/local_role.h>               // LocalRole: the bare no-net tick
#include <runtime/inmatch/loopback_channel.h>          // LoopbackChannel (the host role's own dcb-2 client)
#include <runtime/replication/item_replication_catalog.h> // canonical items.def replication traits
#include <runtime/replication/client_world_materializer.h> // header-only joiner pools 1..3
#include <runtime/inmatch/udp_session_transport.h>     // PeerLink::transport (the LAN per-peer transport)

#include <net/npwire/peer_addr.h>    // PeerAddr / PeerAddrHash
#include "network/udp_pump.h"

#include <formats/mission/bms.h>                      // bms::File (persisted so the host ctx's mission outlives the match)
#include <runtime/inmatch/napi_np_server_ctx.h>     // NapiNPServerCtx / GameConfig / ConnectionMode / SocketMode
#include <runtime/inmatch/napi_np_protocol.h>       // HostAcceptEvent + the host owner-loop entry points
#include <runtime/inmatch/client_runtime.h>         // ClientRuntime (HostClient / Joiner roles)
#include <runtime/inmatch/host_session.h>           // HostOwner + host_session_pump (the shared host owner loop)

#include "simulation/inmatch_session_values.h"
#include "simulation/simulation_asset_state.h"   // the retained asset sources (assets_)
#include "simulation/simulation_net_state.h"     // the net-session shell inputs + sockets (net_)
#include "simulation/simulation_player_state.h"  // the local-player profile state (player_)
#include "simulation/simulation_present_state.h" // the present/occlusion caches (present_)
#include "simulation/deploy_rows.h" // DeployZoneRow / DeployListRow (the DEATH screen feeds)
#include "simulation/hud_view_records.h" // the per-frame HUD view records (typed-array returns)
#include "simulation/present_event_records.h" // the per-tick present drain records (typed-array returns)
#include "devtools/frame_stats.h"

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
class TickSink;

// The Godot adapter for one portable in-match tick target. It owns the World
// and logic systems (WAC VM, BMS evaluator, AI); inmatch::Session owns
// lifecycle, input retention, fixed cadence, and terminal outcomes. One target
// advance is the original's 62 Hz engine tick (current_tick in
// Game_ProcessMainFrame @0x5263f0), while advance_session_frame runs 0..N of
// those, faithful to Game_MainLoop @0x52b630. Per-system cadences live INSIDE
// the systems, as in the original: the WAC VM self-gates to every 62nd tick
// (WacScript_AdvanceTick @0x4f81b1) and the BMS evaluator quarter-passes every
// 16th (Server_TickUpdate @0x51d7e0). MainGame/GameWorld is the sole live
// owner for this path; focused tests and non-gameplay tools may instantiate it
// directly: promote a parsed BMS mission into the world (mission/promote.h),
// register the systems in the faithful order (MissionKernel::finish_load), run
// a pre-mission pass, then tick. Entity transforms (mission -> Godot space)
// and the part-anim phase are exposed for a scene/renderer to draw;
// presentation side effects (text/dialog/win) drain out of the World EffectLog
// each tick. Runtime transport and fixture teardown share the
// play/pause/step/restart surface.
class Simulation : public RefCounted,
                       private opennova::inmatch::TickObserver {
	GDCLASS(Simulation, RefCounted)

public:
	// --- the bound enum/constant surface ------------------------------------
	// The GDScript-visible class-scope enums (BIND_ENUM_CONSTANT needs class
	// scope) re-exporting the engine's values, plus the spawn-origin
	// pack/unpack helpers.
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
		PF_VEHICLE_CTRL_MASK = opennova::world::PF_VEHICLE_CTRL_MASK,
		PF_VEHICLE_TRACK_LEFT = opennova::world::PF_VEHICLE_TRACK_LEFT,
		PF_VEHICLE_TRACK_RIGHT = opennova::world::PF_VEHICLE_TRACK_RIGHT,
		PF_VEHICLE_GUN_YAW = opennova::world::PF_VEHICLE_GUN_YAW,
		PF_VEHICLE_GUN_PITCH = opennova::world::PF_VEHICLE_GUN_PITCH,

		PF_VEHICLE_STEERING = opennova::world::PF_VEHICLE_STEERING,
		PF_VEHICLE_SPEED = opennova::world::PF_VEHICLE_SPEED,
		PF_VEHICLE_ROTOR = opennova::world::PF_VEHICLE_ROTOR,
		PF_VEHICLE_TAIL_ROTOR = opennova::world::PF_VEHICLE_TAIL_ROTOR,
		PF_VEHICLE_WHEELS = opennova::world::PF_VEHICLE_WHEELS,
		PF_VEHICLE_TIRE00 = opennova::world::PF_VEHICLE_TIRE00,
		PF_VEHICLE_TIRE01 = opennova::world::PF_VEHICLE_TIRE01,
		PF_VEHICLE_TIRE02 = opennova::world::PF_VEHICLE_TIRE02,
		PF_VEHICLE_TIRE03 = opennova::world::PF_VEHICLE_TIRE03,
		PF_VEHICLE_TIRE04 = opennova::world::PF_VEHICLE_TIRE04,
		PF_VEHICLE_TIRE05 = opennova::world::PF_VEHICLE_TIRE05,
		PF_VEHICLE_TIRE06 = opennova::world::PF_VEHICLE_TIRE06,
		PF_VEHICLE_TIRE07 = opennova::world::PF_VEHICLE_TIRE07,
		PF_VEHICLE_TIRE08 = opennova::world::PF_VEHICLE_TIRE08,
		PF_VEHICLE_TIRE09 = opennova::world::PF_VEHICLE_TIRE09,
		PF_VEHICLE_TIRE10 = opennova::world::PF_VEHICLE_TIRE10,
		PF_VEHICLE_TIRE11 = opennova::world::PF_VEHICLE_TIRE11,
		PF_VEHICLE_TIRE12 = opennova::world::PF_VEHICLE_TIRE12,
		PF_VEHICLE_TIRE13 = opennova::world::PF_VEHICLE_TIRE13,
		PF_VEHICLE_GEAR = opennova::world::PF_VEHICLE_GEAR,
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
		PF_STANCE_BITS = opennova::world::PF_STANCE_BITS,
		PF_FOCAL_SWAY_VALID = opennova::world::PF_FOCAL_SWAY_VALID,
		PF_FOCAL_SWAY_BASIS_0 = opennova::world::PF_FOCAL_SWAY_BASIS_0,
		PF_FOCAL_SWAY_BASIS_1 = opennova::world::PF_FOCAL_SWAY_BASIS_1,
		PF_FOCAL_SWAY_BASIS_2 = opennova::world::PF_FOCAL_SWAY_BASIS_2,
		PF_FOCAL_SWAY_BASIS_3 = opennova::world::PF_FOCAL_SWAY_BASIS_3,
		PF_FOCAL_SWAY_BASIS_4 = opennova::world::PF_FOCAL_SWAY_BASIS_4,
		PF_FOCAL_SWAY_BASIS_5 = opennova::world::PF_FOCAL_SWAY_BASIS_5,
		PF_FOCAL_SWAY_BASIS_6 = opennova::world::PF_FOCAL_SWAY_BASIS_6,
		PF_FOCAL_SWAY_BASIS_7 = opennova::world::PF_FOCAL_SWAY_BASIS_7,
		PF_FOCAL_SWAY_BASIS_8 = opennova::world::PF_FOCAL_SWAY_BASIS_8,
		PF_FOCAL_SWAY_X = opennova::world::PF_FOCAL_SWAY_X,
		PF_FOCAL_SWAY_Y = opennova::world::PF_FOCAL_SWAY_Y,
		PF_FOCAL_SWAY_Z = opennova::world::PF_FOCAL_SWAY_Z,
		PF_DOOR_COUNT = opennova::world::PF_DOOR_COUNT,
        PF_HUSK = opennova::world::PF_HUSK,
        PF_OBJECT_DESTROY = opennova::world::PF_OBJECT_DESTROY,
        PF_OBJECT_DESTROY01 = opennova::world::PF_OBJECT_DESTROY01,
        PF_OBJECT_DESTROY02 = opennova::world::PF_OBJECT_DESTROY02,
        PF_OBJECT_DESTROY03 = opennova::world::PF_OBJECT_DESTROY03,
        PF_OBJECT_DESTROY04 = opennova::world::PF_OBJECT_DESTROY04,
        PF_OBJECT_DESTROY05 = opennova::world::PF_OBJECT_DESTROY05,
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
	// (static_asserts in simulation_occlusion.cpp pin them).
	enum BlinkFlag {
		BLINK_INDOORS = 0x2,
		BLINK_WATER_OFF = 0x8,
	};

	// The WAC/AI attach-to-seat command ids (world.h SeatSelectionMode maps
	// them to seat filters). [orig: command 123 = sitex only, 124 = reject
	// ctrlx, 125 = any seat — the Entity_RequestVehicleAttach command gates]
	enum MountCommand {
		MOUNT_COMMAND_PASSENGER_ONLY = opennova::world::kCommandAttachPassengerOnly,
		MOUNT_COMMAND_SKIP_CONTROLLER = opennova::world::kCommandAttachSkipController,
		MOUNT_COMMAND_ANY_SEAT = opennova::world::kCommandAttachAnySeat,
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

	// The manual weapon-switch categories request_local_player_weapon_category
	// consumes — the engine's controls::WeaponCategory (runtime/controls/
	// controls.h carries the witness: the 200..210 input cases).
	enum WeaponCategory {
		WEAPON_CATEGORY_KNIFE = static_cast<int>(opennova::controls::WeaponCategory::kKnife),
		WEAPON_CATEGORY_SECONDARY = static_cast<int>(opennova::controls::WeaponCategory::kSecondary),
		WEAPON_CATEGORY_PRIMARY = static_cast<int>(opennova::controls::WeaponCategory::kPrimary),
		WEAPON_CATEGORY_FLASHBANG = static_cast<int>(opennova::controls::WeaponCategory::kFlashbang),
		WEAPON_CATEGORY_FRAG_GRENADE = static_cast<int>(opennova::controls::WeaponCategory::kFragGrenade),
		WEAPON_CATEGORY_SMOKE_GRENADE = static_cast<int>(opennova::controls::WeaponCategory::kSmokeGrenade),
		WEAPON_CATEGORY_ACCESSORY = static_cast<int>(opennova::controls::WeaponCategory::kAccessory),
		WEAPON_CATEGORY_DETONATOR = static_cast<int>(opennova::controls::WeaponCategory::kDetonator),
		WEAPON_CATEGORY_MEDPACK = static_cast<int>(opennova::controls::WeaponCategory::kMedpack),
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
		// static_assert in simulation_bind.cpp).
		ITEM_USER_POINT_SCAN_LIMIT = 16,
		// The retail signed-16 storage domain entity health lives in
		// (world/entity.h kRetailI16Min/Max).
		ENTITY_HEALTH_MIN = opennova::world::kRetailI16Min,
		ENTITY_HEALTH_MAX = opennova::world::kRetailI16Max,
		// The mission-script variable table size — V0..V(N-1), shared by the
		// WAC and BMS evaluators (world/var_store.h ScriptVarStore::kMissionVars
		// carries the witness).
		MISSION_VAR_COUNT = opennova::world::ScriptVarStore::kMissionVars,
		// The horizontal default camera fov, degrees (world/player_view.h
		// kPlayerCameraFovHDeg; the static_assert in the bind TU pins the
		// integral mirror against the engine float).
		DEFAULT_PLAYER_FOV_H_DEG = 80,
		// The retained minimap marker feed's header {version, stride, row_count}
		// and row stride: the engine's one layout (hud/hud_minimap_feed.h),
		// re-exported for the scripting seam the snapshot crosses.
		HUD_MINIMAP_SNAPSHOT_VERSION = opennova::hud::kMinimapFeedVersion,
		HUD_MINIMAP_HEADER_SIZE = opennova::hud::kMinimapFeedHeaderSize,
		HUD_MINIMAP_STRIDE = opennova::hud::kMinimapFeedStride,
	};

	// Spawn-origin provenance (world/entity.h): (kind << 24) | (index &
	// 0xFFFFFF), kSpawnOriginNone = none. Fixed uint32_t so
	// SPAWN_ORIGIN_NONE binds positive.
	enum : uint32_t {
		SPAWN_ORIGIN_NONE = opennova::world::kSpawnOriginNone,
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
	// The engine's ONE mission boot + state + no-net tick (ADR 0042 d3):
	// world, systems (AI/WAC/BMS events/collision/occlusion), the sim asset
	// caches, terrain field store, seat specs, the weapon/ammo tables, and
	// the local-player frame state all live inside. Recreated per load
	// (reset_world) and NEVER null after construction; this binding converts
	// Godot Refs into the kernel's sources and orders device work around it.
	std::unique_ptr<opennova::mission::MissionKernel> kernel_;
	// The private state by owner, each plain data in its own header (ADR 0043
	// d9; the class-body fragments are gone): the net-session shell inputs and
	// sockets, the retained asset sources, the present/occlusion caches, and
	// the local-player profile state. The session, its role, the kernel and
	// the live runtime pointer stay here, on the class that is the embedder.
	SimulationNetState net_;
	// The install root the hosted session's loose _NSTMOUT.TXT reap/pool
	// override is read from at bring-up (HostConfig::game_root, engine
	// session_timeout_config.h): the HostSessionOptions game_root the mission
	// root stamps from the mounted resource root. Empty = no override lookup.
	std::string host_game_root_;
	SimulationAssetState assets_;
	opennova::world::ScriptVoiceChannel::SetResolver voice_set_resolver_;
	SimulationPresentState present_;
	SimulationPlayerState player_;
	using PresentRowIdentity = SimulationPresentState::PresentRowIdentity;
	using PresentEffectPose = SimulationPresentState::PresentEffectPose;
	using CharacterSexRow = SimulationAssetState::CharacterSexRow;
	void _release_weather_owner();
	void apply_collision_to_ai();
	// Portable mission lifecycle and cadence. During one advance call the Godot
	// adapter holds a single typed tick sink so presentation consumes every
	// catch-up tick before the next simulation tick.
	opennova::inmatch::Session session_;
	// The ONE engine role this session runs its ticks through (ADR 0043 d3),
	// constructed per session by kind and never null: a LocalRole from
	// construction, the SP listen server's HostRole(SinglePlayer) by
	// enable_listen_server, the LAN host's HostRole(ListenHost / DedicatedHost)
	// by enable_host_listen, the JoinerRole by enable_join; bound to the kernel
	// at each load. The two typed views name the role's class (null when it is
	// another), set with it: a HostRole is the SP listen server OR a LAN host,
	// so its view is not derivable from session_.kind() alone; a JoinerRole is
	// exactly kind() == Joiner.
	std::unique_ptr<opennova::inmatch::Role> role_;
	opennova::inmatch::HostRole *host_role_ = nullptr;
	opennova::inmatch::JoinerRole *joiner_role_ = nullptr;
	// The C++ TickSink (simulation/tick_sink.h) the presentation owner
	// installs around its own frame call (MissionRoot); null outside one.
	TickSink *session_tick_sink_ = nullptr;
	int64_t frame_net_us_ = 0;
	int64_t frame_sim_us_ = 0;
	int64_t frame_sink_us_ = 0;
	// The dev tools' frame-stats board (ADR 0039): fold_frame_stats() lands the
	// kernel's tick profile (every touched SIM_* slot, ADR 0043 d5) plus the
	// shell-side step/sink spans on it natively at the end of a session frame.
	// Ordinary play leaves runtime_profiling_enabled_ false, so the profile is
	// inactive and no producer reads a clock.
	Ref<FrameStats> frame_stats_;
	void fold_frame_stats(const opennova::inmatch::FrameOutcome &p_outcome);
	opennova::inmatch::Role &active_role();
	// Install a freshly constructed role as the session's (the session's role
	// switch is the gate: false while it is loaded/running, the role stays and
	// the caller's choice waits for the next load). Binds it to the kernel,
	// sets the typed views, follows the live runtime pointer and applies the
	// kind-derived world rules.
	bool install_role(std::unique_ptr<opennova::inmatch::LocalRole> p_role) {
		return adopt_role(std::move(p_role), nullptr, nullptr);
	}
	bool install_role(std::unique_ptr<opennova::inmatch::HostRole> p_role) {
		opennova::inmatch::HostRole *host = p_role.get();
		return adopt_role(std::move(p_role), host, nullptr);
	}
	bool install_role(std::unique_ptr<opennova::inmatch::JoinerRole> p_role) {
		opennova::inmatch::JoinerRole *joiner = p_role.get();
		return adopt_role(std::move(p_role), nullptr, joiner);
	}
	bool adopt_role(std::unique_ptr<opennova::inmatch::Role> p_role,
			opennova::inmatch::HostRole *p_host, opennova::inmatch::JoinerRole *p_joiner);
	// The offline role by the shell's listen_server_ choice: the SP listen
	// server's host role (kind SinglePlayer) or the bare local role.
	bool install_offline_role();
	// The role the next session runs by the shell's choices, re-derived at
	// every load and each choice while the session can switch.
	void ensure_session_role();
	// The ONE writer of the kind-derived world rules (rules.projectile_authority,
	// rules.mp_session): the fresh kernel at reset_world and every role install.
	void apply_session_rules();
	bool begin_session_load();
	void complete_session_load();
	void fail_session_load(const char *p_message);
	void restore_world_baseline();
	// The session's per-tick observer (inmatch::TickObserver): the HUD map death
	// gate after every tick, then the Godot pipeline's tick sink.
	void after_tick() override;
	bool accept_tick(const opennova::inmatch::TickOutcome &p_tick) override;
	// The renderer viewport height a listen host wraps its S2C 0x68 cursor
	// against (0 = headless / no drawable viewport, D-NET-206).
	int32_t renderer_viewport_height() const;
	// The pool-2 building a packed blink hit names, as a bms_id (0 = none).
	int blink_hit_owner_bms_id(uint32_t p_hit) const;
	// Resource-install invariant only. Public lifecycle is
	// session_.state(); this prevents partially constructed worlds from
	// serving data while Loading/Failed transitions are in flight.
	bool world_installed_ = false;
	// An item database handed to resolve_item_traits before any mission is
	// installed (the tool/test order). The next load_from_mission_data installs
	// its table ahead of the boot and sweeps the booted rows, exactly as the
	// file-fed overload does with its explicit database, then drops it: a
	// later load without a new resolve_item_traits gets no stale database.
	Ref<class ItemDatabase> pending_item_traits_db_;

	// --- in-match net runtime (P7, ADR 0009/0011): the SP / LAN host in-process listen server. OFF
	// by default, so an explicit non-network fixture uses the direct AI-pool present. When
	// enabled (before load), the host role's bring-up stands up the npruntime ctx + loopback +
	// HostClient runtime (the role's ListenHostState) and the present pass reads the client-decoded ClientState
	// (ADR 0011 Decision 1) instead of the AI pool. Server_TickUpdate owns the per-frame tick.
	bool listen_server_ = false;
	uint64_t last_sim_tick_us_ = 0;
	uint64_t last_net_tick_us_ = 0;
	// One opt-in gate for every native runtime timer/counter. Retail play keeps
	// this false; F3 Stats and the manual probe share the public ownership seam.
	bool runtime_profiling_enabled_ = false;
	// The FollowOwner effect-pose index over present_ (simulation_present.cpp).
	void invalidate_present_effect_pose_cache() const;
	void ensure_present_effect_pose_cache() const;
	bool cache_present_effect_pose(
			const opennova::replication::ClientEntityState &p_entity_state) const;
	// The host's pool row (D-NET-140: the listen host never presents from ClientState).
	bool cache_present_effect_pose(const opennova::world::Entity &p_entity) const;
	PackedVector3Array cached_present_effect_state_for_handle(uint16_t p_handle) const;
	PackedVector3Array present_effect_state_for_handle(uint16_t p_handle) const;

	// --- co-op LAN joiner: a pure non-authority inmatch::ClientRuntime (the Joiner role enable_join
	// installs; the runtime is built there and rebuilt by the boot's role bring-up; runtime_ is in the
	// P7 block below). The joiner role drives the connect legs + the per-frame S2C->ClientState fold +
	// the C2S 0x0C uplink over a dialed UdpPump; its own player L runs run_logic_tick(false), remotes
	// render wire-direct. (engine: runtime/inmatch/joiner_connection.h) The session kind is the flag.
	// The joiner's per-frame world<->net frame (S10a, ADR 0028; ADR 0043 d3):
	// frame sequence, latches (started/spawned/redeploy/tripwire), the join
	// request, wire-header materializer, per-replica resolver state and every
	// engine leg live on joiner_role_; this binding keeps the loadout profile
	// seams (joiner_kit_seams) and reads the role's observer facts.
	// The joiner's streamed pool-1..3 rows with a placed identity, as the
	// entity dictionaries MissionObjectPlacer.place_entities consumes (kind,
	// index, bms_id, item_id, position, rotation_deg, team, group, ai_flags).
	// Empty on a host or before the world stream's static pools completed.
public:
	Array get_streamed_placement_records() const;

private:
	// The env-gated ~1 Hz tripwire print (the role owns the sampled state and
	// raises the one-shot the observer's after_tick consumes).
	void print_joiner_net_diagnostic_sample();
	// The shell applies the profile kit/class right after runtime setup — on a
	// joiner that is BEFORE L exists (L spawns on the name-match). The class
	// latch lives on the world-typed loadout aggregate
	// (kernel_->local.loadout.pending_player_class); L's spawn block stamps it with
	// the equipped weapon, the same Player_InitPlayer-time arm the host's own
	// spawn performs. (engine: runtime/inmatch/host_session.h)

	// The local-player frame input, look accumulators, stance latch and mouse
	// settings all live on the kernel (kernel_->local.input / look() /
	// request_stance / look_settings); this binding only converts device
	// input and routes the joiner's wire edges.
	// Retail's held-weapon draw gate, local-player branch — the weapon model is shown
	// iff the soldier may fire it. (engine: runtime/inmatch/client_replica_present.h)
	bool local_held_weapon_visible(const opennova::world::Entity &p_entity) const;

	// --- the local player's equipped-weapon action FSM (net-re §5.62) ------------------
	// The 12-state action queue on the equipped slot, pumped once per logic tick after the
	// world advances (engine: runtime/mission/mission_kernel.cpp). The host
	// feeds the baked def via set_local_player_weapon and per-frame trigger state via
	// set_local_player_weapon_input. Presentation outputs accumulate as ordered
	// per-tick records because several logic ticks can run per render frame; the
	// snapshot's monotonic serials remain diagnostics/rebuild state.
	// The whole equipped-weapon state (def/slot/rings/serials/UseGun/
	// PowerThrow/presentation events) lives on the kernel (kernel_->local.weapon,
	// with the retained weapon.def rows and the clip index beside it); this
	// binding marshals installs, inputs, drains, and the two wire request
	// records.
	using LocalUseGunSwitch = opennova::world::LocalUseGunSwitch;
	// The UseGun borrow + slot selection live in world/player_weapon.h; these
	// inline wrappers keep the family's call sites unchanged.
	opennova::world::WeaponSlotState *active_local_weapon_slot() {
		return opennova::world::active_local_weapon_slot(
				kernel_->world, kernel_->local.weapon);
	}
	const opennova::world::WeaponSlotState *active_local_weapon_slot() const {
		return opennova::world::active_local_weapon_slot(
				kernel_->world, kernel_->local.weapon);
	}
	bool local_usegun_switch_is_instant() const {
		return opennova::world::local_usegun_switch_is_instant(
				kernel_->world, kernel_->local.weapon);
	}
	void sync_local_usegun_weapon_transition() {
		opennova::world::sync_local_usegun_weapon_transition(
				kernel_->world, kernel_->local.weapon);
	}
	void commit_local_usegun_weapon_switch() {
		opennova::world::commit_local_usegun_weapon_switch(
				kernel_->world, kernel_->local.weapon);
	}
	void queue_local_usegun_weapon_switch(bool p_same_category) {
		opennova::world::queue_local_usegun_weapon_switch(
				kernel_->world, kernel_->local.weapon, p_same_category);
	}
	void install_local_player_weapon(const Ref<WeaponDef> &p_def,
	                                 const Dictionary &p_clip_seconds,
	                                 bool p_preserve_slot_state,
	                                 bool p_allow_same_weapon_rebake);
	// The Dictionary/def-row feeders both build the world install payload.
	static opennova::world::WeaponInstallData install_data_from_def(
			const opennova::def::DefWeaponDef &p_def, const Dictionary &p_clip_seconds);

	// --- the local player's weapon slot pool + spawn kit + map rules -------------------
	// The slot pool, spawn kit, availability table and pre-spawn class latch
	// all live on the kernel (kernel_->local.inventory / kernel_->local.loadout, with the
	// world-typed rules in world/player_loadout.h); this binding converts
	// dictionaries and routes the joiner wire submissions. The player-scoped
	// weapon profile record and its seed cursor live on player_.
	// The switch commit/outcome/gates moved to world/player_weapon.h (S7a);
	// wrappers keep the family's call sites unchanged.
	void commit_pending_weapon_switch() {
		opennova::world::commit_pending_weapon_switch(
				kernel_->world, kernel_->local.weapon,
				kernel_->local.inventory_valid ? &kernel_->local.inventory : nullptr);
	}
	void handle_weapon_switch_outcome(
			const opennova::world::WeaponSwitchOutcome &p_out) {
		opennova::world::handle_weapon_switch_outcome(
				kernel_->world, kernel_->local.weapon,
				kernel_->local.inventory_valid ? &kernel_->local.inventory : nullptr, p_out);
	}
	opennova::world::WeaponSwitchGates local_weapon_switch_gates() const {
		return opennova::world::local_weapon_switch_gates(
				kernel_->world, kernel_->local.weapon,
				kernel_->local.inventory_valid ? &kernel_->local.inventory : nullptr);
	}
	// Player_InitPlayer's weapon leg (engine: runtime/inmatch/host_session.h); shared by table load,
	// respawn, and the ACCEPT apply (which passes the freshly stored kit).
	void rebuild_local_player_loadout(bool p_select_spawn_default);
	// Copies the assigned side's profile page into the resident kit buffer
	// (kernel_->local.loadout.spawn_kit) in a live session — retail's single restrictionData (engine: runtime/inmatch/loadout_submit.h). False when
	// not in a session, before the catalog exists, or when the page resolves empty.
	bool seed_session_kit_from_profile();
	// Re-copies the page when the SIDE the team selector names stops matching the side
	// the resident buffer came from — the S2C 0x04 latch arriving after the catalog, or
	// a later S2C 0x50 reassignment (engine: runtime/inmatch/joiner_connection.cpp).
	bool reseed_session_kit_on_side_change();
	// Push the submission content (class + ADM rows + equipped combo) into the joiner
	// runtime's 0x2F loadout-submission seam. No-op for hosts/SP.
	void push_joiner_loadout_kit();
	bool apply_local_player_loadout_impl(
			const TypedArray<WeaponKitEntry> &p_kit, int p_player_class,
			bool p_submit_joiner_request);
	// The typed-rows core of the apply (the record overload converts, the 0x5A
	// grant path feeds inmatch::kit_from_authoritative_grant's rows directly).
	bool apply_local_player_loadout_rows(
			std::vector<opennova::world::WeaponKitEntry> p_kit,
			int p_player_class, bool p_submit_joiner_request);
	// Fold the latest authoritative S2C 0x5A grant into the local slot pool at
	// the same recv-before-actions boundary as the retail handler.
	void apply_joiner_authoritative_loadout();
	// The deploy/spawn-zone registry (letters/pick-index space) on net_, built
	// lazily per load (engine: runtime/hud/hud_lfp_panel.h).
	const opennova::world::SpawnZoneRegistry &deploy_zone_registry();
	// The DEATH screen's zone rows over the registry: the record feed and
	// the compiled list builder both read this one walk.
	std::vector<opennova::world::DeployZoneRow> deploy_zone_rows();
	// --- the local player's view state (ADS ease + 3P anchor chase) --------------------
	// The view state and its trackers live on the kernel (kernel_->local.view /
	// kernel_->local.view_tracker; the witnessed gates in world/local_player_view.h);
	// this class converts frames and routes wire requests
	// (simulation_player_view.cpp).
	void reset_local_player_view_effects();
	void refresh_local_player_view_effects();
	// The BMS input-action word (world::ScriptState::input_action_bits, the
	// cat-7 player-trigger evaluator's LIVE word) as the view-action producers
	// have written it; 0 without a kernel. A test seam over the engine state.
	int debug_input_action_bits() const;
	// The dead-player map-mode clear, run once per advanced tick (witness at
	// hud::HudMapControl::on_local_player_dead).
	void tick_hud_map_death_gate();
	// The loadout profile seams the joiner role keeps shell-side (the role is
	// constructed with them).
	opennova::inmatch::JoinerRole::KitSeams joiner_kit_seams();

	// --- P7: the in-match runtime as a THIN ADAPTER over engine/runtime/inmatch ----------------
	// One in-match runtime funnels every live path: the host/SP game is the §5.0 mode-3
	// listen server (the host role's NapiNPServerCtx + its own loopback client, driven by
	// the npruntime owner loop = Server_TickUpdate + tick_connections + handle_server_datagram);
	// the joiner is a non-authority inmatch::ClientRuntime. The Godot net bindings stay PURE socket
	// pumps — all protocol/crypto/framing lives in libs (ADR 0009-0012, .agents/network.md).
	// The SP/LAN listen session's net state (inmatch::ListenHostState) lives on the host
	// role: the loopback + the np host owner the ONE listen frame (inmatch::HostRole::run_tick)
	// drives — ctx + per-peer transports + now_tick + serve_and_play, shared with
	// host_session_pump (engine/runtime/inmatch). Null when this sim runs no host role, so
	// every reader guards (an absent host reads exactly like an idle one).
	opennova::inmatch::ListenHostState *host_state();
	const opennova::inmatch::ListenHostState *host_state() const;
	opennova::inmatch::NapiNPServerCtx *host_ctx();
	const opennova::inmatch::NapiNPServerCtx *host_ctx() const;
	// The active role's HostClient (host/SP) OR Joiner runtime; the present-snapshot source.
	// A binding member (ADR 0042 d3: no headless joiner consumer; the binding also folds the
	// host's own view with its perf clocks).
	opennova::inmatch::ClientRuntime *runtime_ = nullptr;
	// The decode-view item-class resolver over the retained catalog (empty before
	// resolve_item_traits built one): what a host role is constructed with.
	opennova::replication::ClientReplicaPipeline::ItemClassResolver item_class_resolver() const;
	// Install the catalog resolver on the host role and runtime_'s view (no-op until the
	// catalog exists). Called from resolve_item_traits, the boot's role hook, and enable_join.
	void install_item_class_resolver();
	// Install or clear the retained boot charattr table on the current Joiner runtime.
	void install_charattr_challenge_table();
	// Copy the per-class ATTRIBUTES words into World::class_attribute_flags -- the
	// joiner's live table when one exists (S2C 0x41 mutates it in receive order),
	// else the boot copy. Runs at world creation, at every table install, and
	// after each net pump (engine: runtime/inmatch/charattr_challenge.cpp).
	void sync_class_attribute_flags();
	// Install the retained retail player-profile join block on the current runtime.
	void install_character_join_vars();
	// Install or clear the explicitly selected retail integrity corpus profile.
	void install_join_integrity_profile();
	// Install the retained JOIN-checksum install root (D-NET-166).
	void install_expansion_version_root();
	// Install the retained APPID join token (decoded .joi CK) on the current runtime.
	void install_app_id();
	// Install the retained CD identity cookie (packed PUB* blob) on the runtime.
	void install_join_cd_cookie();
	// The per-load host bring-up record: mode 3 -> create_session(&host_loop) [connection-table
	// reset + Server_InitNewRoundState] -> the faithful host-player auto-spawn, over the kernel's
	// world/mission. Mirrors apps/nw_server; the Godot-fed context installs (mission text,
	// .til bytes, GameConfig from the UI host config) sit beside the shared core. Staged on the
	// host role right before the kernel boots; the role's bring_up consumes it.
	opennova::inmatch::HostBringup host_bringup();
	// The per-frame host owner loop: the ONE inmatch::HostRole::run_tick over the kernel
	// (drain -> pre-tick -> host_session_pump -> local pumps -> adm ground), with the
	// binding supplying the viewport-height seam, the local ClientState fold + perf
	// clocks, and the local reload relay.
	// The per-frame non-authority client loop is the joiner role's frame
	// (S10a, ADR 0043 d3); the pre-mission preload frame is the role's too.

	// The terrain/sound/character legs over assets_ (the kernel owns the one
	// cpt/trn(+charmap) field store, ADR 0042 d4).
	void apply_terrain_to_ai();
	void apply_sound_state_to_world();
	void apply_character_traits_to_world();
	// Resolve the session's score row from the mission's game-mode bit
	// (either load order is legal).
	void refresh_score_rules();
	// The shared install tail (both install orders): sort for the per-frame
	// binary search, stamp turret clamps, refresh live pool-1 rows, and re-sync
	// the header-only materializer image.
	void finalize_installed_seat_specs();
	void refresh_item_seat_spec(opennova::world::Entity &p_entity);
	// Resolve each spec's turret clamp window from its primary weapon's
	// weapon.def rows. Called from BOTH install orders (specs-then-table and
	// table-then-specs); all-zero = not authored, no clamp.
	void stamp_seat_spec_turret_limits();

	void reset_world();


public:
	// --- the weather home (world::WeatherState, ADR 0042 d2/d5) --------------
	// The World's weather, the ONE home the WAC handlers write, the kernel's
	// weather tick advances, the 0x0A projection serializes and a joiner's
	// decoder writes back. Null without a kernel; C++ seams for the sibling
	// native nodes (the Weather node binds its render owner here).
	opennova::world::WeatherState *weather_state();
	const opennova::world::WeatherState *weather_state() const;
	bool weather_state_bound() const { return weather_state() != nullptr; }
	// The mission-start seed (the embedder's ONE derivation, env::weather_seed_from_config).
	void seed_weather(const opennova::world::WeatherSeed &p_seed);
	// The render owner the kernel's weather tick calls after the sim legs
	// (null detaches); remembered so the World's death releases the owner's
	// pointer before the environment can read a freed WeatherState.
	void set_weather_render_owner(Weather *p_owner);
	// Complete the native mission-start boundary after the weather seed.
	bool complete_mission_start();
	// The precipitation drop pool's per-render update + compile for a camera
	// (renderer/precipitation_frame.h carries the cites): the compiled frame the
	// Precipitation node streams (drops, five floats per vertex, color, snow);
	// an empty frame without a world. C++-only, the node is its one consumer.
	const opennova::renderer::PrecipitationDrawFrame &compile_precipitation_frame(
			const Vector3 &p_camera, const Vector3 &p_camera_right,
			const Vector3 &p_camera_up, int p_terrain_light_rgb);
	// Thunder one-shots since the last drain (weather_state.h carries the cites).
	// NOT ClassDB-bound: MissionAudio plays the engine rows.
	void drain_weather_sounds(std::vector<opennova::world::WeatherSoundEvent> &r_events);
    void drain_script_sounds(std::vector<opennova::world::ScriptSoundEvent> &r_events);
	// The F3 Environment record (devtools/environment_snapshot.h) as a typed
	// read for the GUT/probe side; null without a world.
	Ref<EnvironmentSnapshot> get_environment_snapshot() const;
	// The F3 Environment window's record (ADR 0042 d6): the ENGINE join over
	// the weather home; false without a world.
	bool native_environment_snapshot(opennova::devtools::EnvironmentSnapshot &out) const;
	// The MCP/debug rows' authority-gated weather commands (the F3 window
	// reaches EntityCommands natively through DevTools). False on a joiner.
	bool command_rain(int p_percent, int p_seconds);
	bool command_snow(int p_percent, int p_seconds);
	bool command_overcast(int p_percent, int p_seconds);
	bool command_fog_distance(int p_metres);
	bool command_move_fog(int p_metres, int p_seconds);
	bool command_sky_speed(int p_rate);
	bool command_quake(int p_seconds);
	bool command_time_of_day_minutes(int p_minute_of_day);
	// The exact dev-tool scrub (not the WAC `tod` math).
	bool debug_set_time_of_day_minutes(double p_minute_of_day);
	bool command_fog_type(int p_type);
	bool command_lightning_flash();
	bool command_lightning_far_flash();
	bool command_wind_scale(int p_value);
	bool command_sky_height(int p_height_raw);
	bool command_sun_fade(int p_percent, int p_seconds);
	bool command_color_fade(int p_seconds);
	// One weather color block (world::WeatherColorTarget index, packed
	// 0xRRGGBB) and the lightning color.
	bool command_weather_color(int p_target, int p_rgb);
	bool command_lightning_color(int p_rgb);

private:
	// The shared post-kernel-boot binding legs: session-header capture, HUD
	// map zoom, score-row re-resolve, and the held-WacProgram re-apply.
	void finish_kernel_boot();
	// The kernel boot's bringup_net_session hook for this sim's role.
	std::function<void()> role_bringup_hook();
	void apply_host_session_mission_header(const opennova::bms::File &file);

protected:
	static void _bind_methods();

public:
	Simulation();
	// A dying joiner sim ships the retail goodbye burst before the socket drops — retail sends
	// its disconnect packets from the connection teardown that Destroy also runs, so freeing the
	// sim (ESC abort, watchdog abort, return-to-menu) must not leak an admitted peer on the host.
	// (engine: runtime/inmatch/client_runtime.cpp)
	~Simulation() override;

	// Load + promote an in-memory bms::File. This remains a narrow fixture/tooling seam;
	// ONED gameplay launches only from a saved loose .bms (GameWorld.load_mission).
	bool load_from_mission_data(const Ref<MissionData> &p_mission);
	// S9 (ADR 0028): the ordered mission boot — engine/runtime/mission
	// runtime_boot owns the sequence + the file-resolution policy; this entry
	// supplies the step bodies over the existing feeds. The shell composes
	// role bring-up before it and presentation after it. The caller finishes
	// startup WAC after seeding weather (Weather::run_mission_start_boundary).
	// Returns OK or
	// ERR_CANT_OPEN (mission missing / load failed; the sequence aborted).
	int64_t boot_mission(const Ref<MissionData> &p_mission,
			const Ref<ResourceRoot> &p_resource_root,
			const Ref<ItemDatabase> &p_item_db,
			const Ref<TerrainData> &p_terrain,
			const PackedByteArray &p_terrain_til, const String &p_wac_basename,
			const String &p_infantry_adm, const String &p_mission_file_basename,
			bool p_playable);
	// Build + promote a small synthetic patrol mission (no file) for the headless unit test.
	void build_demo_mission();
	bool is_loaded() const;

	// Transport.
	bool is_playing() const {
		return session_.state() == opennova::inmatch::State::Running;
	}
	// Advance exactly ONE 62 Hz logic tick — the original's engine tick. The per-system
	// dividers gate INSIDE the systems (the WAC VM self-gates to every 62nd tick, the BMS
	// evaluator quarter-passes every 16th), exactly where the original keeps them. Returns
	// false when the session cannot take a direct local/test tick. Banking wall
	// clock and dispatching 0..N ticks per render frame belongs to inmatch::Session
	// (the Game_MainLoop @0x52b630 accumulator) — a render frame is NOT one tick.
	// (engine: formats/sph/sph.h)
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
	bool is_host_listening() const {
		const opennova::inmatch::RoleKind kind = session_.kind();
		return kind == opennova::inmatch::RoleKind::ListenHost ||
				kind == opennova::inmatch::RoleKind::DedicatedHost ||
				net_.lan_host_pending;
	}
	int get_host_listen_port() const;  // the bound UDP port (0 when not listening)
	int get_host_peer_count() const;   // joiners in handshake or admitted
	// The hosted-session request (network/host_session_options.h): every
	// user-facing field lands; the sim-owned fields (the mission header blob,
	// the score tables) are untouched. Call BEFORE loading a mission.
	void configure_host_session(const Ref<HostSessionOptions> &p_options);
	// A snapshot of the live host session as the same record.
	Ref<HostSessionOptions> get_host_session_config() const;
	// The BMS header the tag=0x0B join reply carries: 0 until a mission loaded.
	int get_mission_header_size() const;
	// Debug/test hook: directly admit a synthetic remote peer at a Godot-space
	// position, exercising the admit_peer + connection wiring without a live
	// socket handshake (the handshake itself is unit-tested in libs —
	// tests/npruntime/handshake_server_test, the P2 retarget of the retired
	// novaworld host_session_accept_test). Returns true if an entity was
	// spawned + bound. No-op unless host listening is on.
	bool admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team);

	// --- co-op LAN joiner (D.2) -------------------------------------------
	// Turn the sim into a co-op LAN JOINER: dial the host and run the witnessed in-match JOIN
	// as a non-authority client. `player_name` rides the game ClientAuth.NA — the key the host
	// echoes into our organic-spawn record (name-match self-ID, wire handle H). Call BEFORE
	// loading the mission; a sim is host XOR joiner; false when the socket can't be dialed.
	// `join_role` 1 = the retail spectator role (ClientAuth JSR=1 + optional JSPP).
	bool enable_join(const String &p_host_ip, int p_port, const String &p_player_name,
			int p_join_role = 0, const String &p_spectator_password = String(),
			const String &p_server_password = String(),
			const String &p_join_password = String());
	bool is_joiner() const { return session_.kind() == opennova::inmatch::RoleKind::Joiner; }
	// Placed identities retired since the last take (the slot vanished or was
	// re-typed): the mission root hides their placed representation.
	PackedInt32Array take_retired_placement_ids();
	// Live player-slot spectator state: joiner = S2C 0x75 latch; authority = Server_SetPlayerSpectator.
	bool is_local_spectator() const;
	bool set_local_spectator(bool p_spectator);
	// The client-local death screen latch (retail g_death_screen_active): gates the
	// friendly-tags walks + camera arbiter sub-mode; fed by simulation_player_view.cpp.
	bool local_death_screen_active() const;
	// True while a live net session owns this sim: the world tick is the ONLY pump for the
	// session socket, so the Play/Step/Stop transport locks out (retail MP has no pause; a
	// stopped listen host reaps every joiner at cs_dir0.timeout_ms (engine: net/novaworld/client_session.h)). The single home for the rule — F3 transport, MCP, and ESC pause read it.
	bool is_transport_locked() const { return is_joiner() || is_host_listening(); }
	// The portable session's live role/state records (runtime/inmatch/session.h),
	// re-exported so the debug/MCP shell derives authority and role labels from
	// the session instead of re-deriving them from the transport flags. The
	// role values mirror inmatch::Role (the assignments make drift impossible);
	// session_state() reports inmatch::State in the same values
	// MissionFrameOutcome.STATE_* carries.
	enum SessionRole {
		ROLE_SINGLE_PLAYER = static_cast<int>(opennova::inmatch::RoleKind::SinglePlayer),
		ROLE_LISTEN_HOST = static_cast<int>(opennova::inmatch::RoleKind::ListenHost),
		ROLE_JOINER = static_cast<int>(opennova::inmatch::RoleKind::Joiner),
		ROLE_DEDICATED_HOST = static_cast<int>(opennova::inmatch::RoleKind::DedicatedHost),
	};
	int session_role() const { return static_cast<int>(session_.kind()); }
	int session_state() const { return static_cast<int>(session_.state()); }
	// The fixed logic-tick quantum (1/62.5 s) — the ONE cadence constant,
	// re-exported from the engine accumulator for GDScript composition.
	static double tick_dt() { return opennova::world::TickAccumulator::kTickDt; }
	// The tick cadence as a rate, and wall-clock ms -> whole logic ticks —
	// re-exports of the engine tick home (world/tick_accumulator.h carries
	// the current_tick witness).
	static int ticks_from_ms(int64_t p_ms) {
		return opennova::world::ticks_from_ms(p_ms);
	}
	// The epilog/debrief ESC-less exit timeout in seconds, derived from the
	// engine tick constants (world/world.h kEpilogExitTimeoutTicks).
	static double epilog_exit_timeout_seconds() {
		return opennova::world::kEpilogExitTimeoutTicks *
				opennova::world::TickAccumulator::kTickDt;
	}
	// The epilog/debrief screen fade-in in seconds (world/world.h
	// kEpilogFadeInTicks, the 48+48-tick cine fade pair).
	static double epilog_fade_in_seconds() {
		return opennova::world::kEpilogFadeInTicks *
				opennova::world::TickAccumulator::kTickDt;
	}
	// The DEATH deploy screen's content refresh cadence in seconds
	// (world/deploy_screen_feed.h kDeployRefreshTicks).
	static double deploy_refresh_interval_seconds() {
		return opennova::world::kDeployRefreshTicks *
				opennova::world::TickAccumulator::kTickDt;
	}

	// --- Portable session frame (ADR 0035) --------------------------------
	// The input and outcomes are typed values. The one temporary tick sink
	// (simulation/tick_sink.h, installed by the presentation owner around its
	// own frame call) keeps per-tick Godot presentation synchronous during
	// catch-up without installing a persistent callback bus; the GameWorld leg table
	// orders concrete devices around this call.
	Ref<MissionFrameOutcome> advance_session_frame(
			const Ref<MissionFrameInput> &p_input);
	Ref<MissionFrameOutcome> step_session_frame(
			const Ref<MissionFrameInput> &p_input);
	// The C++ per-tick sink: MissionRoot IS the sink and binds itself for the
	// duration of each advance/step call (null between calls, so a direct
	// step() or a stray holder of this sim never reaches a dead owner).
	void set_tick_sink(TickSink *p_sink) { session_tick_sink_ = p_sink; }
	bool pause_session();
	bool resume_session();
	bool reset_session();
	void close_session();
	// The dev tools' board: SIM_STEP and the SIM_* phase slots are fed here
	// while the profiling clocks run and the board captures.
	void set_frame_stats(const Ref<FrameStats> &p_stats);
	Ref<FrameStats> get_frame_stats() const;
	int64_t get_last_session_sim_us() const { return frame_sim_us_; }
	// Set the per-side character ids/classes/avatar bytes carried by ClientAuth.
	// Must be called before enable_join; later runtime rebuilds retain the values.
	// The joiner's / the listen host's own two-side character selection
	// (AvatarDatabase.character_join_profile); null installs nothing.
	void set_join_character_profile(const Ref<CharacterJoinProfile> &p_profile);
	void set_local_character_profile(const Ref<CharacterJoinProfile> &p_profile);
	// Select a registered retail resource-corpus profile for S2C 0x30/0x31.
	// Empty clears it; an unknown id also clears it and returns false.
	bool set_join_integrity_profile(const String &p_profile_id);
	// The game-session APPID join token the client recovers from the NWJoin .joi
	// CK; a NovaWorld host validates it (reject code 9). Empty/"0" is the LAN
	// default. Retained across runtime rebuilds like the character/integrity data.
	void set_app_id(const String &p_token);
	// The CD identity cookie (packed PUB* blob) for the C2S 0x00 JOIN — the
	// NovaWorld-issued NAMEINFO/PCID/SQUADINFO/JOINTICKET the host validates
	// (codes 23/24/25/28). Empty for LAN. Retained across runtime rebuilds.
	void set_join_cd_cookie(const PackedByteArray &p_cookie);
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
	// The S2C 0x81 hit-confirm edge: {} unless a positive/negative score delta
	// landed since the last take, else {score, delta, tone} with the tone name
	// ("" / "HITTONE" / "KILLTONE" / "HEADSHOTTONE") the presenter plays as a
	// 2D interface sound behind the enable_slotmachine setting
	// (engine: runtime/replication/client_state.h).
	Ref<ScoreFeedback> take_score_feedback();
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
	// (engine: net/novaworld/client_session.h)
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
	// The per-second joiner trace is deliberately opt-in for release play: the
	// `net_joiner_diagnostics` debug control (F3 / MCP game_debug) switches it
	// on. The snapshot remains available so tests/debug UI can distinguish a
	// real ordered gap from ordinary idle traffic.
	bool is_joiner_network_diagnostics_enabled() const;
	void set_joiner_network_diagnostics_enabled(bool p_enabled);
	Dictionary get_joiner_network_diagnostics() const;
	// The pcap this session's datagrams are recorded to (`--capture-pcap`,
	// LaunchFlags); applied to the pump when the host binds or the joiner
	// dials, so set it before the session opens. "" records nothing.
	void set_capture_pcap_path(const String &p_path);
	// Player-paced deployment (the deploy-map screen; net-re §5.61/§5.0d). True while
	// the join owes the player a deployment pick or awaits the host's release of one —
	// the shell shows the DEATH deploy screen and the join watchdog stops (the
	// remaining transitions are player-paced).
	bool is_join_deploy_pick_pending() const;
	// The deploy-map overlay signal (retail g_deploy_screen_active). UI only.
	bool is_join_deploy_overlay_active() const;
	// The frame loop's open decision for death.mnu's DEATH screen: true once
	// per arming of the overlay (the engine-side open latch stamps itself and
	// clears when the host drops the bit). The shell calls it only while no
	// other screen is up and opens the presenter on true.
	bool take_join_deploy_overlay_open();
	// The DEATH screen's spawn-zone rows (simulation/deploy_rows.h): one
	// DeployZoneRow per team-owned deploy zone with its SECURED verdict, wave
	// countdown and occupants, letters/names keyed by the spawn-zone registry
	// index. Row 0 (the Default Spawn, param 0) is the shell's.
	// (engine: runtime/replication/client_state.h)
	TypedArray<DeployZoneRow> get_deploy_spawn_zones();
	// The compiled SPAWNPOINTS_LIST rows (DeployListRow): the engine builder's
	// two witnessed loops (world/deploy_screen_feed.h) over the zone rows
	// above, the team colour tag, the Menu default-row tokens and the
	// embedder-resolved WPNames strings (name_key -> text). value 0 = default,
	// index+1 = zone, -1 = occupant/blank (never a pick).
	TypedArray<DeployListRow> get_deploy_list_rows(const String &p_default_key,
			const String &p_default_home, const Dictionary &p_zone_names);
	// The DEATH screen's STATIC facts: the 0x0A sub-block-0 timers, the queued
	// wave line, the psp/medic show gates, and the medic-call cooldown.
	Ref<DeployStatus> get_deploy_status(const Ref<RtxtStringFile> &p_gametext);
	// The dead player's medic call (C2S 0x2E): gated on a dead local player and
	// the 310-tick cooldown; a joiner queues it, the listen host loops it back.
	// (engine: runtime/inmatch/client_runtime.h)
	bool request_local_player_medic();
	int local_medic_request_cooldown_ticks() const;
	int local_medic_request_serial() const;
	// The rtxt "Server" table's STRSRV_MEDREQ format for the host's broadcast.
	void set_server_text(const String &p_medic_request_format);
	// The one role-agnostic read of the local player's dead bit.
	bool local_player_dead() const;
	// The end-of-round presentation feed (net-re §5.68; simulation_end_round.cpp):
	// the 0x1D header edge + the 0x56 board through the ONE ClientEndRoundStats
	// every role's view folds; the overlay text ladder (hud/end_round_overlay.h)
	// and the stat.mnu RESULTLIST columns/rows (npruntime/stat_screen_feed.h).
	Ref<EndRoundState> get_end_round_state() const;
	// The retail is_in_session fact for the shell's round-cycle and HUD
	// arms: the world's mp_session bit (the 0x1D header form).
	bool is_mp_session() const;
	// The ladder's input from this role's view (C++ only, not bound): the
	// producer behind get_end_round_overlay.
	opennova::hud::EndRoundOverlayInput end_round_overlay_input() const;
	// The overlay ladder resolved through the gametext Overlays table (the
	// folds + printf forms are the engine's end_round_overlay_resolve):
	// texts/ys/top/bottom ready for HudOverlay::set_end_round_overlay.
	Ref<EndRoundOverlay> get_end_round_overlay(const Ref<RtxtStringFile> &p_gametext) const;
	// The RESULTLIST columns with their header text resolved through the
	// gametext Overlays table (null gametext = the "!..." fallbacks).
	TypedArray<EndRoundColumn> get_end_round_columns(int p_table_width,
			const Ref<RtxtStringFile> &p_gametext) const;
	// The rows, filtered by stat.mnu's tab (0 all, 1 team 2, 2 team 1 — the
	// engine's stat_screen_row_visible).
	TypedArray<EndRoundRow> get_end_round_rows(int p_tab) const;
	// hud::kEndRoundStatScreenDelayMsec — the 6 s stat.mnu delay.
	static int end_round_stat_screen_delay_msec();
	// hud::strip_inline_tags — retail's `<...>` markup stripper.
	static String strip_inline_tags(const String &p_text);
	// The SP Show Score statistics counters (hud/end_round_statistics.h):
	// the 0xC846xx block the toggled panel draws. Null when no host world.
	Ref<EndRoundStatistics> get_end_round_statistics() const;
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
	// The session g_GameType word an SP/offline mission implies: the mission's attrib mode
	// through the catalog's for_mission_mode map (no multiplayer bit -> stock Co-op 0x10020).
	// The listen host seeds its GameConfig from it before the auto-spawn, the same word the
	// LAN-host dialog derives on the GDScript side (HostSessionConfig.game_type_auto).
	// (engine: base/gameprofile/game_type.h)
	uint32_t mission_game_type() const;
	// Spawn the host's own player at the mission's player-START marker, selected the way the
	// original engine does — by game type, FARTHEST from the enemy set — NOT at any NPC's
	// position (net-re §5.2c). A stock SP mission resolves the Co-op 6094 -> 6001 chain. Call AFTER a
	// mission is loaded. Returns: 1 = spawned at a real start marker; 0 = no start marker, spawned
	// at a safe fallback origin (never an NPC); -1 = failed (no mission / pool 0 full).
	// (engine: runtime/inmatch/server_message_dispatch.cpp)
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
	// selection (engine: formats/def/def.h).
	void set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right,
	                      bool p_lean_left, bool p_lean_right, bool p_jump);
	// One frame of mouse pixels (screen sense: +x right, +y down) applied to the local
	// player's look through the witnessed integer pipeline: sens = setting << 11,
	// scoped zoom reduction, (px*sens+0x8000)>>16 per axis; yaw wraps; pitch clamps
	// +-80 deg with the up-limit +40 deg while prone. (engine: runtime/mission/mission_kernel.cpp)
	void add_local_player_look(float p_dx_px, float p_dy_px);
	// Mouse options: sensitivity (default 128; copied unclamped like the profile
	// apply — the [1,511] range is the +/- adjust's); Y invert (flipmouse, default off).
	// (engine: runtime/world/player_look.h)
	void set_local_player_mouse(int p_sensitivity, bool p_invert_y);
	// Stance SELECT request (0 stand / 1 crouch / 2 prone) — the 3-key semantics: each
	// key selects its stance, mutual exclusion at apply, REFUSED while the equipped
	// weapon has ForceCrouch (0x40000). Returns whether the stance changed. (engine: runtime/inmatch/client_runtime.h)
	bool request_local_player_stance(Stance p_stance);
	// The local player's authoritative position in Godot world space (for the follow camera);
	// Vector3() when no player is spawned.
	Vector3 get_local_player_position() const;
	// The local player's live eye offset above its position (the entity CameraOffset
	// mirror the USE seat scan measures from), Godot space; Vector3() when no player is spawned.
	Vector3 get_local_player_eye_offset() const;
	// The AI row's 16.16 position (mission x/y ground, z up) the retail hashes read; false without a player row.
	bool local_player_position_q16(int32_t (&r_pos)[3]) const;
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
	// per-frame HUD info. (engine: runtime/hud/hud_frame.h)
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
	// (engine: runtime/world/player_loadout.cpp)
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
	// [orig: RenderSlot_DrawAllDrapes @0x5d6e81 reads
	// g_PlayerStanceProneLatch, see docs/render/render-lighting-re.md].
	Stance get_local_player_stance_latch() const { return static_cast<Stance>(kernel_->local.stance_latch()); }
	// The HUD stance icon index (0 stand / 1 crouch / 2 prone) from the sim's
	// authoritative stance state [orig: HUD_BuildEntityInfo @0x4b860c —
	// entity+300 flags 0x200=crouch -> 1, 0x100=prone -> 2]. The witnessed
	// source is the body state, never the anim clip name.
	Stance get_local_player_stance() const;
	int get_local_player_anim_phase_ticks() const;
	String get_local_player_anim_source_key() const;
	int get_local_player_anim_source_phase_ticks() const;
	float get_local_player_anim_blend_weight() const;
	// The local player's third-person aim-overlay state — the torso bend. Dictionary:
	//   valid: bool; aim_state: bool (anim-state flag 0x40 — the bend branch);
	//   body: Vector3 mission-euler degrees (pitch, yaw, roll) for the avatar node basis;
	//   angles: PackedVector3Array[9] mission-euler degrees per anim::OverlayClass.
	// The blends run in exact BAM int math (engine: runtime/anim/aim_overlay.cpp); the shell converts each triple with
	// MissionObjectPlacer.bms_to_godot_basis (the single-sourced frame conversion) and
	// feeds ObjectModel.set_aim_overlay. Empty/invalid when no player.
	Ref<PlayerAimOverlay> get_local_player_aim_overlay() const;
	// The third-person held-weapon model name for an ADM index (weapon.def gfx3).
	String get_weapon_third_person_model(int p_adm_index) const;

	// --- the local player's equipped-weapon FSM (net-re §5.62) -------------
	// Install the equipped weapon: p_def is the WeaponDatabase weapon dict (the
	// {actions, flags, clipsize, startrounds} slice is consumed) and p_clip_seconds
	// maps each .adm clip key to its VARIANT lengths in SECONDS — a
	// PackedFloat32Array in .adm file order (SkeletalAnim.get_clip_variant_lengths;
	// a plain float is accepted as a single-variant convenience). The lengths seed the
	// per-slot rings and the Anim_InitActions bake consumes them ring-wise: one
	// serve-then-advance read per 'auto' delay field (engine: runtime/world/player_weapon.cpp). A normal install is a real mount and
	// resets the personal slot unless p_preserve_slot_state selects an already-live
	// UseGun parent/personal slot.
	void set_local_player_weapon(const Ref<WeaponDef> &p_def, const Dictionary &p_clip_seconds,
	                             bool p_preserve_slot_state = false);
	// The production mount (S6b, ADR 0028): find the row in the RETAINED
	// weapon.def parse, bake the FSM def from it, and seed the clip rings from
	// the rig's own .adm through the sim's mounted index — one step at ACCEPT
	// time, no shell dictionary and no render dependency (engine: runtime/simassets/adm_clip_index.h). Returns false
	// when the name is not in the retained table (caller keeps the current
	// weapon, mirroring the armory guard). The Dictionary pair above survives
	// as the GUT synthetic-def seam and retires with S7a.
	bool install_local_player_weapon_by_name(const String &p_weapon_name,
	                                         bool p_preserve_slot_state = false);
	// Render-side late binding of .adm clip lengths for the already-mounted def.
	// This is the only path allowed to preserve a same-name live action slot and
	// queued presentation (engine: runtime/inmatch/joiner_role.cpp).
	void rebake_local_player_weapon(const Ref<WeaponDef> &p_def,
	                                const Dictionary &p_clip_seconds,
	                                bool p_preserve_slot_state = false);
	void clear_local_player_weapon();
	void set_local_player_first_person_model_available(bool p_available);
	// Whether the last push found a first-person model (a read seam for the
	// GUT viewmodel pins; the presenter reads the def, not this).
	bool is_local_player_first_person_model_available() const;
	// Per-frame trigger state: fire held + edge, raw reload edge (the dispatch
	// gate runs sim-side) (engine: runtime/world/local_player_view.cpp).
	void set_local_player_weapon_input(bool p_fire_held, bool p_fire_pressed,
	                                   bool p_reload_pressed);
	// The ADS toggle request: gated by the dispatcher rules (no toggle during
	// RELOAD/SWITCHFROM, def Flags & 3 required), flips the sim-owned engaged bit
	// and queues the scopeup/scopedown FSM states. Returns whether it toggled.
	// (engine: runtime/world/local_player_view.cpp)
	bool request_local_player_scope_toggle();
    void set_local_player_aspect_mode(int p_mode);
    int get_local_player_aspect_mode() const;
	bool request_local_player_scope_zero(int p_delta);
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
	// viewchase -> true) (engine: runtime/world/local_player_view.cpp); the
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
	Ref<PlayerLocalView> get_local_player_view() const;
	// Horizontal -> vertical projection fov (degrees) through the aspect — the
	// ONE conversion both cameras use (engine: runtime/world/player_view.cpp).
	static float fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect, int p_mode = -1);
	// The first launch's seed of the aspect mode (the display_16x9 cfg word)
	// from the primary desktop's size: 1 past the 1.34 ratio line, else 0
	// (engine: runtime/renderer/aspect_ratio.h).
	static int fresh_profile_aspect_mode(int p_width, int p_height);
	// The presentation frame's view forward for mission-euler angles, the aim
	// ray's far point and the binocular rangefinder readout — the engine's
	// world/presentation_frame.h (no presenter spells the swizzle).
	static Vector3 presentation_forward(float p_yaw_deg, float p_pitch_deg);
	static Vector3 aim_ray_endpoint(const Vector3 &p_eye, float p_yaw_deg, float p_pitch_deg);
	static int rangefinder_units(const Vector3 &p_position, const Vector3 &p_endpoint);
	// The FP viewmodel rig's frame math (simassets/fp_viewmodel_spec.h): the
	// view-frame -> camera-local axis map, the def rotation bias as camera
	// euler radians, the rig yaw, the renderfov default, the TEX_TEAM byte.
	static Vector3 viewmodel_camera_local_from_view(const Vector3 &p_view_units);
	static Vector3 viewmodel_bias_euler_rad(const Vector3 &p_rot_bias_deg);
	static float viewmodel_rig_yaw_deg();
	static float weapon_render_fov_h_deg_default();
	static int viewmodel_team_byte(int p_team);
	// The waypoint-track snapshot for the HUD label: {show, count, current,
	// number, name_id, position (Godot space), done}. current is -1 with no
	// selection; number is the 1-based display index [orig: hudInfo+373 =
	// list index + 1 @ 0x4b88e8]. Read-only; the track advances in the world
	// tick. (docs/interface/hud-re.md §Waypoint HUD)
	Ref<WaypointHudView> get_waypoint_hud_view() const;
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

	// The type-2043 grid-origin marker: present + Godot-space position.
	Ref<HudMapGridOrigin> get_hud_map_grid_origin() const;
	// The objectives-panel rows for header slots 1..8, terminated at the
	// first 0/255 win-condition id — exactly the panel's row walk
	// (engine: runtime/mission/promote.cpp) — filled natively for
	// HudOverlay::set_objectives: the SHOWN rows with their mission-text
	// lines resolved through the table. NOT ClassDB-bound.
	void fill_objectives(const Ref<RtxtStringFile> &p_mission_text,
			std::vector<opennova::hud::HudObjectiveRow> &r_rows) const;
	// The FSM snapshot for the shell: latest clip/action payloads, diagnostic serials,
	// ammo, kick, and the 3P body channel. Ordered presentation events drain through
	// drain_local_player_weapon_events(); the snapshot alone is not an event queue.
	Ref<PlayerWeaponView> get_local_player_weapon_state() const;
	// Destructively drain the ordered presentation outputs accumulated since the
	// previous render frame. Each Dictionary encodes one PlayerWeaponEvent.
	TypedArray<PlayerWeaponEvent> drain_local_player_weapon_events();
	// Destructively drain the flight sim's resolved round impacts, each row already
	// mapped through the ammo effects_table to its effect and sound legs
	// (engine: runtime/world/ammo_table.h). The native form is the C++
	// consumer's (GameWorld); the bound form wraps the same rows for the tests.
	void drain_round_impact_rows(std::vector<opennova::world::RoundImpactPresentation> &r_rows);
    void drain_script_effects(std::vector<opennova::world::ScriptEffectEvent> &events);
    std::vector<std::string> script_effect_names() const;
    void bind_item_effect_scene(std::shared_ptr<opennova::particle::EffectScene> scene);
	TypedArray<RoundImpactRow> drain_round_impacts();
	// Destructively drain permanent terrain-cache scorch insertions (mission
	// 16.16 bounds; the consumer folds mission (x,y) to terrain/Godot (x,z)).
	// NOT ClassDB-bound.
	void drain_terrain_scorches(std::vector<opennova::world::TerrainScorchEvent> &r_events);
	// Drain this frame's folded S2C 0x1E game events as typed feed rows — one
	// per line the original posts to its message feed. The fold (suppression,
	// the own/verbose gate, the camp keys, the bonus recompose, the color) is
	// the engine's (runtime/hud/feed_format.h feed_event_rows); the actor
	// names resolve here, where the decoded roster lives. The embedder
	// resolves each row's keys against gametext and calls the format helpers
	// below.
	TypedArray<FeedRow> drain_feed_events();
	void retain_feed_announcement(const String &text, int64_t tick);
	String get_kill_announcement_text() const;
	int64_t get_kill_announcement_tick(int64_t now);
	// The folded Tab board's HEADER (netsim ClientScoreboard counts + the
	// session strings): known/team_mode/timed, the witnessed players count
	// (accepted rows minus the spectator trailer, replication::scoreboard_header),
	// in_game/spectators, game_type, server and mission names. The rows no
	// longer round-trip through script — HudOverlay pulls them natively via fill_scoreboard_rows.
	Ref<ScoreboardHeader> get_scoreboard() const;
	// The native Tab-board row handoff: fills the drawer's entries via the
	// netsim projection (replication::project_scoreboard — wire order, the server
	// sorts and the client never re-sorts). NOT ClassDB-bound; HudOverlay
	// calls it through this typed seam. Returns false (rows cleared) when no runtime exists.
	bool fill_scoreboard_rows(
			std::vector<opennova::hud::ScoreboardEntry> &r_rows) const;
	// The folded board's team-table count (netsim ClientScoreboard::team_count,
	// the host's configured side count as the 0x16 carries it); 0 without a
	// runtime. NOT ClassDB-bound; HudOverlay reads it beside the rows.
	int scoreboard_team_count() const;
	// The mounted-vehicle panel (hud/hud_vehicle_panel.h, world/vehicle_panel_feed.h):
	// {shown, item_id} — the panel's root vehicle (the attached gun child
	// re-roots to its parent), whose items.def sid the shell joins to its
	// VEHICLE_HUD block. Read-only.
	Ref<VehiclePanelView> get_vehicle_panel_view() const;
	// The native panel handoff (NOT ClassDB-bound; HudOverlay::set_vehicle_panel
	// calls it): the hull's health band + one row per authored seat pair
	// (occupancy, rider health, the seat-select digit, the own seat). Returns
	// false (rows cleared) when the local player rides nothing.
	bool fill_vehicle_panel(const opennova::def::DefVehicleHudBlock &p_block,
			opennova::hud::HudVehiclePanelState &r_state) const;
	// The AAS zone status panel feed (world/lfp_feed.h), NOT ClassDB-bound:
	// one HudLfpZone per spawn-zone list entry joined with the client
	// runtime's zone-timer entry and the zone's transient minimap slot flags.
	bool fill_lfp_zones(int p_local_team,
			std::vector<opennova::hud::HudLfpZone> &r_zones);
	// The in-match game type for every role (joiner header / HostClient view).
	int64_t get_session_game_type() const;
	// The S2C 0x14 player-chat lines since the last drain, each routed by the
	// witnessed channel table (ChatLineRow.sink: 0 = the SYSTEM ring, 1 = the
	// CHAT ring, 2 = the message queue, 3 = channel 3).
	TypedArray<ChatLineRow> drain_chat_lines();
	// Substitute actor names into a canned template (engine: runtime/replication/client_replica_feed.cpp): the STRCND48 bonus re-compose when `extra` names the local
	// player, then $A/$B sequential case-insensitive replace-all. Exposed so
	// the string lookup can live with the string table while the substitution
	// rule stays in engine C++.
	String format_feed_line(const String &p_template, const String &p_attacker,
			const String &p_victim, const String &p_extra,
			const String &p_bonus_template) const;
	// Compose a camp line — the template's %s takes the level's WPNames string
	// (engine: runtime/hud/feed_format.cpp).
	String format_feed_camp_line(const String &p_template,
			const String &p_wpname) const;

	// --- the local player's loadout: slot pool, spawn kit, map rules -------------------
	// (the 2026-07-18 loadout grill; witness map in docs/net/novaworld-net-re.md §5.57)
	// The spawn kit (engine: formats/mission/bms.h):
	// rows {name, ammo_primary, ammo_secondary, flags} (values default -1). When
	// p_filter_by_availability, the kit is filtered through the availability table
	// with the knife fallback — the SP .bms promote leg (engine: runtime/world/player_loadout.cpp); the armory/profile legs store unfiltered (the server validates).
	// An empty kit resets to the engine default {WPN_M4AUTO} (engine: runtime/mission/mission_kernel.cpp).
	void set_spawn_loadout(const TypedArray<WeaponKitEntry> &p_kit, bool p_filter_by_availability);
	// True only after a mission/profile explicitly supplied a spawn kit; the
	// WPN_M4AUTO engine fallback created by load_weapon_table leaves this false.
	bool has_explicit_spawn_loadout() const { return kernel_->local.loadout.spawn_kit_set; }
	// Availability by weapon name: 0 banned / 1 allowed / 2 armory-zone-only /
	// 3 mission-allowed; unknown names read 1. The armory UI filter term
	// (engine: runtime/world/weapon_inventory.h).
	int get_weapon_availability(const String &p_weapon_name) const;
	// The armory ACCEPT apply (engine: runtime/inmatch/loadout_submit.cpp): the accepted kit becomes the spawn kit, the slot pool refills from it
	// (sub-weapons expanded), pools reseed + clips recalc, and the equipped slot
	// re-selects. Rows whose weapon is availability-banned are refused (the server
	// 0x2F validation shape, availability 2 requires the armory zone the ACCEPT is
	// gated on anyway [orig: @ 0x515a4a]). Also stamps player_class when 5..9.
	bool apply_local_player_loadout(const TypedArray<WeaponKitEntry> &p_kit, int p_player_class);
	// Commit the profile class without replacing a mission-authored weapon kit.
	bool set_local_player_class(int p_player_class);
	// Load the player's weapon profile (weapon.sav) from an ABSOLUTE filesystem path.
	// This is a save file, not a mounted PFF/loose resource, so it is read through
	// FileAccess rather than the resource root. Header gate: magic "FPBC" + version
	// "0211", then five 0x1080C profile-slot records; slot 0 becomes the active
	// record and its class bytes are clamped to [5,9]. A missing or malformed file is
	// NOT fatal — the shipped defaults stay installed and an Error is returned so the
	// caller can warn. (engine: base/gameprofile/required_resources.c)
	Error load_weapon_profile(const String &p_path);
	// Read slot 0's two character headers (raw bytes, no session clamp) without
	// requiring a live Simulation: a WeaponProfileSummary (error, loaded, the
	// blue and red WeaponProfileSide with player_class, avatar_a (nationality
	// id), avatar_b (division id) and avatar_packed). This is the menu boot
	// seam over the same five-record file as load_weapon_profile().
	static Ref<WeaponProfileSummary> read_weapon_profile_summary(const String &p_path);
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
	// "expansion/<name>/", never the root (engine: formats/playersav/weapon_sav.cpp). Static so shell path assembly stays a join.
	static String weapon_profile_relpath(const String &p_expansion_name);
	// The FP viewmodel submit spec {gun, arms, adm, show_arms} (simassets
	// fp_viewmodel_spec (engine: runtime/inmatch/joiner_role.cpp)). `character_arms` is the local
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
	static Ref<FpViewmodelSpec> fp_viewmodel_spec(bool p_has_def, const String &p_gfx1,
			const String &p_character_arms, const String &p_animadm, int p_flags);
	// Rebuild the local player's slot pool from the spawn kit and select the spawn
	// default — the Player_InitPlayer weapon leg (engine: runtime/inmatch/host_session.h). Runs automatically after load_weapon_table; call
	// again on respawn.
	void respawn_local_player_loadout();
	// The category keys (engine: runtime/controls/controls.h). Category 1..9 =
	// the retail Knife/Sidearm/Primary/Flashbang/Frag/Smoke/Accessory/Detonator/
	// Medpack keys ('1'..'9').
	void request_local_player_weapon_category(WeaponCategory p_category);
	// Next/previous weapon (engine: runtime/controls/controls.h).
	void request_local_player_weapon_cycle(int p_direction);
	// Inventory snapshot for hosts/tests (simulation/player_inventory.h).
	Ref<PlayerInventory> get_local_player_inventory() const;
	// Canonical, unexpanded current tuples for the armory host. Retail preselects
	// visible parent rows from g_armoryLoadoutBufferByClass, never from the expanded
	// weaponSlotArrayBase [orig: populate_ammo_type_combo_boxes @ 0x564930].
	TypedArray<WeaponKitEntry> get_local_player_loadout() const;

	// --- WAC scripts ------------------------------------------------------
	// Install a compiled program on the script VM (WacProgram, C++-only since
	// the ADR 0043 d10 sweep). Applied now if loaded and re-applied on every
	// (re)load. Pass null to uninstall.
	void set_wac_program(std::shared_ptr<WacProgram> p_program);
	// Compile `sources` against the LIVE promoted world (symbolic group/area names
	// resolve through the registry) and install on success. False (program not
	// installed) when compilation has errors; the retained WacProgram holder
	// carries the diagnostics. (C++-only; the compile surface and the installed
	// program's execution are pinned by the wac_program_surface ctest.)
	bool compile_and_set_wac(const PackedStringArray &p_sources);
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
	int64_t get_last_net_tick_us() const { return static_cast<int64_t>(last_net_tick_us_); }
	int64_t get_last_present_snapshot_us() const {
		return static_cast<int64_t>(present_.last_snapshot_us);
	}
	int64_t get_last_occlusion_build_us() const {
		return static_cast<int64_t>(present_.last_occlusion_build_us);
	}
	int64_t get_last_occlusion_probe_us() const {
		return static_cast<int64_t>(present_.last_occlusion_probe_us);
	}
	// Script-disable gate [orig: dword_C6EB28].
	void set_wac_paused(bool p_paused);
	bool is_wac_paused() const;

	// Drain the World EffectLog as MissionEffect records and clear it.
	// Presentation-only (text/dialog/win/subgoal/show_waypoints/set_light);
	// state mutation is applied in-engine, never here.
	TypedArray<MissionEffect> drain_effects();

	// The shell fire-presentation drain: one FirePresentationRow per round
	// spawned since the last call, with the ammo-def 'ai_launch'/'ai_launcheffect'
	// names resolved. The fire present pass spawns per event, skipping the local
	// player (whose action-slot presentation is already ported). The native
	// form is the C++ consumer's (FirePresenter); the bound form wraps the
	// same rows for the tests. (engine: runtime/world/ai.h)
	void drain_fire_presentation_rows(std::vector<opennova::world::FirePresentationRow> &r_rows);
	TypedArray<FirePresentationEvent> drain_fire_presentation_events();

	// The fire-sound legs on the logic clock (world/fire_sound.h): the shell
	// stamps the camera listener each frame before the tick batch, and drains
	// the ready one-shots each present (NOT ClassDB-bound; the fire pass reads
	// the engine rows). (engine: runtime/world/collision.h)
	void set_sound_listener(const Vector3 &p_listener_godot);
	void drain_fire_sounds(std::vector<opennova::world::ReadyFireSound> &r_sounds);

	// The eased FP viewmodel view-offset in VIEW-FRAME world units (X=fwd,
	// Y=left, Z=up) from raw weapon.def pos/tpos units — the /256 blend +
	// NoCardSwitch suppression run in world/player_view (S8), then the
	// per-frame motion lead (the damped movement-delta tracker) and the 0x500
	// z drop when the viewport frames 4:3 or narrower — the rig samples the
	// viewport SIZE (device work) and the 3w<=4h rule itself is the engine's
	// (world/player_view.h player_view_narrow_aspect). The rig maps view axes
	// onto its camera frame. (engine: runtime/world/local_player_view.cpp)
	Vector3 local_player_viewmodel_bias_view_units(
			const Vector3 &p_pos_raw_units, const Vector3 &p_tpos_raw_units,
			int p_viewport_w, int p_viewport_h);

	// The sound-profile chain (engine: base/gameprofile/required_resources.c): feed SndProf.def text (VFS
	// bytes) — parsed into world.tables.sound_profiles now and re-applied on
	// reset_world; per-entity bindings resolve in the kernel boot's
	// simassets::resolve_ai_weapons step.
	void set_sound_profiles(const PackedByteArray &p_sndprof_text);
	// The mission water plane (godot Y units) the footstep water pick and the
	// landing legs compare feet against (engine: runtime/replication/client_replica_pipeline.h).
	void set_water_z(double p_water_y);
	// Drain the per-tick slot-sound emissions (footsteps/foley/landing/screams),
	// played by the fire present pass at full volume; NOT ClassDB-bound
	// (engine: net/npwire/ingame_decode.h).
	void drain_slot_sounds(std::vector<opennova::world::SoundSlotEvent> &r_events);

	// Queue this frame's REMOTE-body footsteps and foley for one wire row.
	// Runs only for wire-RENDERED bodies (a joiner's remote rows, a listen
	// host's admitted players); the authority tick's sound pass never reaches
	// net-snapped peers, so each drawn body has exactly one source. The
	// applier supplies the row's identity, the wire-driven clip playhead span
	// it just crossed, and its world pose; the witnessed consume itself is
	// the portable world::wire_body_slot_sounds (world/wire_body_sound.h),
	// fed through the same SoundSlotEvent drain the authority bodies use
	// [orig: the org1/org2 sound blocks, see docs/audio/lwf-dbf-sound-re.md].
	void present_wire_body_sounds(int p_type_id, int p_character_id,
			int p_wire_handle, int p_carrier_handle, int p_anim_state,
			int p_from_phase, int p_to_phase, const Vector3 &p_pos);
	// Drain persistent entity-attached emitter registrations. Producers refresh
	// a keyed (source_spawn_id, lane) intent; the audio layer expands the set
	// into LWF layers and owns keep-alive, spatial ranking, and physical voices.
	// (engine: runtime/world/sound_emitter_mailbox.h) The native form is the
	// fire pass's; the bound form wraps the same rows for the tests.
	void drain_sound_emitter_events(std::vector<opennova::world::SoundEmitterEvent> &r_events);
	// Typed audio presentation/acknowledgement seam; WAC owns the channel state.
	opennova::world::ScriptVoiceChannel::Frame script_voice_frame(const Vector3 &p_listener);
	void finish_script_voice(uint64_t p_serial, const opennova::lwf::WavPcm *p_clip);
	void set_script_voice_resolver(opennova::world::ScriptVoiceChannel::SetResolver p_resolver);
	bool play_script_wave(const String &p_filename);
	TypedArray<SoundEmitterRow> drain_sound_emitters();

	// The live tracer TRAIL channels — the per-round point rings behind every streak,
	// framed per channel as [style_id, age, count, then count x (x, y, z, w)] in
	// godot space; the friendly/enemy style is already selected at spawn vs the local
	// team, and killed rounds' channels keep draining until empty. The fire present
	// pass builds the camera-facing ribbons from these.
	// (engine: runtime/world/tracer_trails.h)
	PackedFloat32Array get_tracer_trails() const;

	// The in-flight round glows: one row per active round whose ammo authors
	// `light_move`. The presenter's light pool spawns a permanent (mode 1) light per
	// id, follows it per tick, and despawns dropped ids [orig:
	// RoundData_SpawnRound @0x4ec8da spawn, the per-tick follow @0x4eaa9f,
	// Projectile_ReleaseEffects clear — witness map on engine/runtime/renderer/light_scene.h].
	// NOT ClassDB-bound: EffectLightDirector reads the engine rows.
	void fill_round_glows(std::vector<opennova::world::RoundGlowRow> &r_rows) const;

	// The destruction presentation drain (world/destruction.h; world-wac-ai-re
	// §24): the effect/sound/husk-swap/death-light rows since the last drain
	// plus the diagnostic counters, moved out and cleared (empty until a world
	// is installed). NOT ClassDB-bound: DestructionPresenter reads the engine
	// events; the tests author a DestructionDrain through its data leg.
	void drain_vehicle_effects(std::vector<opennova::world::VehicleEffectEvent> &r_events);
	void drain_destruction_events(opennova::world::DestructionEvents &r_events);
	// The live death-piece pool — each piece renders as its single husk-model
	// section. NOT ClassDB-bound. (engine: runtime/world/destruction.cpp)
	void fill_death_pieces(std::vector<opennova::world::DeathPieceRow> &r_pieces) const;
	// Whether the collision world holds an instance for the placed entity
	// `bms_id` — the one destruction-gate fact the GUT collision cases read
	// (the item-trait banks themselves are pinned by the
	// item_death_traits_resolve ctest). False for an unknown id.
	bool has_collision_instance(int p_bms_id) const;

	// Mission scripting state on the shared world (the dword_C6B240 var store + event gates).
	void set_mission_variable(int index, int value);
	int get_mission_variable(int index) const;
	bool has_event_fired(int index) const;
	int get_event_count() const;

	// --- Read-only introspection (dev tools / MCP tooling) -----------------
	// The world's logic tick counter (engine: base/io/tick_rate.h). The
	// pre-mission pass in finish_load already advanced it once, so a freshly
	// loaded mission reads 1 — consumers should track deltas, not absolutes.
	int64_t get_logic_tick() const;
	void set_panm_time_ms(int64_t p_time_ms);
	int64_t get_panm_time_ms() const;
	void debug_set_panm_time_ms(int64_t p_time_ms);
	// The installed mounted-pose model sources (the seat/mount table's
	// resolved graphics per type): the count the seat-install pins read.
	int get_mounted_graphic_source_count() const;
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
	// The typed entity inspection API (world::inspect, ADR 0042 d5). The join
	// and both card halves are computed engine-side; this binding forwards and
	// converts into typed records — JSON conversion lives on the records
	// themselves and runs only at the MCP boundary (to_json_value). A joiner's
	// directory carries no AI join (the non-authoritative tooling pool never
	// mixes into the decoded view), and its cards ride the decoded replica
	// section (inmatch::client_replica_card).
	TypedArray<EntityRow> entity_directory() const;
	// Native (unbound) form for the in-process C++ dev tools (ADR 0042 d6): the same engine
	// join, returned as the engine vector — no TypedArray/Variant round-trip. Empty without a kernel.
	std::vector<opennova::world::inspect::EntityRow> native_entity_directory() const;
	// The engine's tool/probe mutation seam by entity handle (ADR 0042 d5), for the C++
	// embedders (DevTools) that already hold a handle; null without a kernel.
	opennova::world::EntityCommands *entity_commands();
	// Native (unbound): the engine card by value for the C++ dev tools; invalid without a kernel
	// or a resolving handle.
	opennova::world::inspect::EntityCard native_entity_card(int p_handle) const;

	// --- the F3 Weapon window's native seams (devtools; ADR 0042 d6) -------
	// Engine-typed records out (no Variant round-trip); null/-1 without a
	// kernel or nothing equipped. The retained row carries the AUTHORED delays
	// (-1 = `auto`); the ACTIVE slot is the UseGun-borrowed one when engaged;
	// the input block names the pump's gate ("" = accepted). Full contracts:
	// simulation_player_weapon.cpp.
	const opennova::world::LocalPlayerWeapon *native_local_player_weapon() const;
	const opennova::def::DefWeaponDef *native_equipped_weapon_row() const;
	int native_equipped_weapon_adm_index() const;
	const opennova::world::WeaponSlotState *native_active_weapon_slot() const;
	const char *native_weapon_input_block() const;
	std::vector<std::string> native_equipped_weapon_clip_keys() const;
	// Live ACTION edits — never the filesystem. Delays arrive AUTHORED and
	// mirror into the retained row so a re-install keeps them; only explicit
	// legs patch the live baked slot; `p_rebake` (a leg newly `auto`) or an
	// ANIM change takes the same-weapon re-bake that keeps the live slot,
	// serials and scope. p_field/p_trigger pair by static_assert at the drain.
	bool debug_weapon_set_action_delays(int p_action_id, int p_delay_start, int p_delay_end,
			bool p_rebake);
	bool debug_weapon_set_action_text(int p_action_id, int p_field, const String &p_text);
	// Queue an action through the REAL input seam; false when its gate refuses.
	bool debug_weapon_trigger(int p_trigger);
	void debug_weapon_set_fire_held(bool p_held);
	bool debug_weapon_fire_held() const { return player_.debug_weapon_fire_held; }
	// Arm, disarm and clear the pump's 62.5 Hz trace ring.
	void debug_weapon_arm_trace(bool p_armed);
	void debug_weapon_clear_trace();
	// The full card by packed wire handle; null when nothing resolves. The AI-index and SSN forms
	// wrap the same builder (edit seams key on ai_index; pool-1 vehicles have no brain, resolve by SSN).
	Ref<EntityCard> entity_card(int p_handle) const;
	Ref<EntityCard> entity_card_by_ai_index(int p_index) const;
	Ref<EntityCard> entity_card_by_net_id(int p_net_id) const;
	// Probe seam: write an AI entity's health via the scripted-SETHP stores (registry + motor
	// copy) so in-game probes can shorten a fight. Returns ERR_UNAVAILABLE without a live sim,
	// ERR_INVALID_PARAMETER for a missing AI index, and OK only after both mirrors are mutated.
	Error debug_set_entity_health(int p_index, int p_hp);
	Error debug_crew_vehicle(int p_occupant_ssn, int p_vehicle_ssn);
	void set_local_player_eye_offset(const Vector3 &p_offset_godot, bool p_valid);
	bool local_player_fp_weapon_hidden() const;
	Error debug_crew_local_player(int p_vehicle_ssn);
	// Authority test seam: queue a RoundDeath for the player entity at `handle` (killer = the
	// local player) so the next host tick runs the witnessed death transaction
	// (route_round_deaths: 0x13 fan, 0x52 camera, 0x54 medic state, the dead flag on the 0x0A
	// record). Not the health setter above: remote players are not AI rows.
	Error debug_kill_player_entity(int p_handle);
	// Probe seam: teleport an AI entity (mission-space coords) through both position stores, for
	// probes defeated by mission geography. Same truthful Error contract as debug_set_entity_health.
	Error debug_set_entity_position(int p_index, const Vector3 &p_mission_pos);
	// World-registry probe seam by SSN: mission-space teleport (the by-SSN
	// entity card is entity_card_by_net_id above).
	void debug_set_world_entity_position(int p_net_id, const Vector3 &p_mission_pos);
	// Exact-slot parity probe: set the authoritative MountSlot words on a
	// world entity so a real UDP phase-8 sample can prove receiver application.
	Error debug_set_world_entity_weapon_ammo(int p_net_id, int p_clip,
	                                         int p_reserve);
	// Per-entity items.def attrib override by packed wire handle (brainless rows included):
	// EntityCommands::set_entity_item_attrib. ERR_UNAVAILABLE without a kernel,
	// ERR_INVALID_PARAMETER for a handle/word out of range, ERR_DOES_NOT_EXIST when nothing resolves.
	Error debug_set_entity_item_attrib(int p_handle, int64_t p_attrib, int64_t p_attrib2);
	// Land the local player at an exact F3-dumped pose (probe seam). Returns
	// ERR_UNAVAILABLE until the complete local-player subject exists.
	// TEST SCAFFOLDING (host authority): kill a BMS command group outright so an
	// unattended round can reach a scripted win an autofiring bot cannot. Drives the
	// same EntityCommands::kill_group the BMS KILL_GROUP action uses; returns members
	// affected, or -1 with no world.
	int debug_kill_group(int p_group);

	Error debug_teleport_local_player(const Vector3 &p_mission_pos, float p_yaw_deg,
			float p_pitch_deg);
	// Round-outcome card: {ended, winner_team, bluekills, greenkills, enemy_kills,
	// team_kills_by_others, friendly_kills_by_others, enemy_kills_by_others, humans}.
	// The sim-side end-of-round state + the SP kill-stat buckets the epilog score
	// screen and the WAC bluekills/greenkills builtins read (probe + HUD source).
	// (engine: runtime/world/ai.h)
	Ref<RoundOutcome> get_round_outcome_debug() const;
	// Human-readable AI state name, "?" for the id gaps (engine: runtime/world/ai.h).
	static String ai_state_name(int p_state);
	// Infantry anim state id -> ADM clip key ("anim_<off_8135F0 name>"), empty for invalid gaps.
	static String infantry_anim_key(int p_state);
	// The adjacent retail transition-arbitration flags table (off_8139E8).
	static int64_t infantry_anim_flags(int p_state);

	// Entity query. The (kind, index) pair lets the shell map a sim entity back to
	// its promoted mission record and already-rendered node.
	int get_entity_count() const;
	int get_entity_kind(int p_index) const;         // mission ItemType (3 = Organic), -1 if none
	Vector3 get_entity_position(int p_index) const; // mission (x,y,z) -> Godot (x, z, -y), units
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

	// ONE batched present snapshot for the per-tick render pass: a flat PackedFloat32Array of
	// get_entity_count() records, PF_STRIDE floats each, fields per the PresentField enum. Avoids the
	// ~10 Variant-boxed scalar getter calls per entity the present loop would otherwise make.
	PackedFloat32Array get_present_snapshot() const;
	// The door side table the most recent get_present_snapshot() built beside
	// its rows: (row index, count, phase[count]) int32 entries in row order,
	// only for rows whose PF_DOOR_COUNT is nonzero (runtime/inmatch/present_rows.h).
	PackedInt32Array get_present_door_phases() const;
	// Revision for the exact ordered identity layout of the most recently
	// returned snapshot. Pose-only changes keep this stable.
	int64_t get_present_layout_revision() const {
		return static_cast<int64_t>(present_.layout_revision);
	}
	int get_present_stride() const { return PF_STRIDE; }

	// Wire the terrain the AI grounds on (the shell's loaded TerrainData). Copies the depth
	// buffer + sector layout so the portable height field outlives the source and survives reload.
	// Null/unloaded clears grounding (entities keep their authored Z). GameWorld
	// and direct test/tooling fixtures call this through MissionRoot.setup().
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

	// Install the default infantry root-motion map (e.g. "E_STAND.adm") through
	// the shell's resource root. Returns its number of states with usable clips
	// (0 if unavailable); model-specific maps resolve independently. Clip sets
	// survive reset_world like the terrain.
	int set_infantry_anim_map(const Ref<class ResourceRoot> &p_resource_root, const String &p_adm_name);
	// Usable states in the first registered map (default or model-specific).
	int get_infantry_clip_count() const { return kernel_->root_motion.clip_count(0); }

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
	// (engine: runtime/simassets/item_traits.cpp)). Idempotent; call after load (and again after
	// spawning the local player).
	void resolve_item_traits(const Ref<class ItemDatabase> &p_item_db);

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
	// alignment chase, climb states, exits) (engine: runtime/replication/client_replica_pipeline.cpp).
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
	// (engine: runtime/mission/runtime_boot.h)
	void occlusion_init_mission();

	// The per-render-frame occlusion pipeline: building batch + portal slots +
	// occluder planes + the section-mask build + the per-entity render gates
	// (blink-hits + the outdoors three-ray latch). Camera in Godot space; fov_y in
	// degrees; fog/water in mission units; force_indoors mirrors the mission
	// attribute override (engine: formats/mission/bms.h).
	// (engine: runtime/world/occlusion.cpp)
	void run_occlusion_frame(const Transform3D &p_camera, double p_fov_y_deg,
	                         double p_aspect, double p_near, double p_fog_dist_units,
	                         double p_water_z_units, bool p_force_indoors);

	// Frame results: [bms_id, packed] pairs for every building the occlusion
	// frame touched; the packed word is world/occlusion_feed.h's
	// pack_building_visibility (section mask low, visible flag at bit 32),
	// read back through the two static decoders below. C++-only (the frame
	// consumes the delta form; the full form stays for native callers).
	PackedInt64Array get_building_visibility() const;
	// The section mask of a packed building verdict (bit N = COBJ section /
	// render part N; bit 0 = exterior; forced-visible def bits merged).
	static int64_t building_visibility_mask(int64_t p_packed);
	// The batch/frustum visible flag of a packed building verdict.
	static bool building_visibility_visible(int64_t p_packed);
	// bms_ids of non-building entities the collector gates culled this frame
	// (C++-only, like get_building_visibility).
	PackedInt32Array get_render_culled_bms_ids() const;
	// Delta form of get_building_visibility(): only pairs whose packed value
	// changed since the last call, so the shell applies changes instead of
	// re-walking the whole building set every frame.
	PackedInt64Array get_building_visibility_changes();
	// Delta form of get_render_culled_bms_ids():
	// [n_added, ids..., n_removed, ids...] since the last call.
	PackedInt32Array get_render_culled_changes();
	// The same delta over the decoded rows the EntityPresenter wire walk
	// draws (culled wire handles this frame against the applied baseline).
	PackedInt32Array get_wire_render_culled_changes();
	// Per-draw sun-visibility feed (D-RLIT-3): triples
	// [wire_handle_or_-1, bms_id_or_0, quality 1..4] whose quality changed.
	// Exactly one identity is live per row. This is a cutover API: no bms-only
	// pair form remains. The shell maps quality through sun_visibility_factor;
	// the wire presenter applies it to late-built bodies and held weapons too.
	// The local player's quality is
	// computed but never emitted here — the presenter reads it via
	// get_local_player_sun_quality() so the FP parts can keep their witnessed
	// exemption while the third-person body dims.
	PackedInt64Array get_draw_lighting_changes(const Vector3 &p_light_dir);
	int get_local_player_sun_quality() const { return present_.local_sun_quality; }
	// Quality (1..4) -> the effectScale the render-state stack multiplies —
	// engine-owned so the mapping has ONE writer (renderer::
	// sun_visibility_factor carries the Entity_ComputeSunVisibility cite,
	// see docs/render/render-lighting-re.md). Both
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

	// The F3 AI window's pushed record (native, unbound): the ONE engine join,
	// world::inspect::ai_debug_report, as the engine struct with no Variant
	// round-trip. False without a kernel or on a joiner (the tooling AI pool
	// never joins the decoded view).
	bool native_ai_debug(opennova::world::inspect::AiDebugReport &r_out) const;
	// Engine ray-debug capture (CollisionWorld rings + engine-owned mask/TTL
	// draw filter) behind the F3 Rays window; counts + filter state ride
	// native_rays_snapshot (ADR 0042 d6). Filter setter: -1 keeps a value.
	// C++-only: DevTools drives these and reads the unbound snapshots.
	void set_ray_debug_recording(bool p_enabled);
	void set_ray_debug_filter(int64_t p_mask, int64_t p_ttl_ticks);
	void clear_ray_debug();
	bool native_rays_snapshot(opennova::devtools::RaysSnapshot &out) const;
	// Engine contact-debug capture (the CollisionWorld hit/contact ring) behind
	// the F3 Physics window; counts + capture state ride
	// native_physics_snapshot (ADR 0042 d6). Mask setter clamps to the kinds.
	void set_contact_debug_capture(bool p_enabled);
	void set_contact_debug_kind_mask(int64_t p_mask);
	void clear_contact_debug();
	bool native_physics_snapshot(opennova::devtools::PhysicsSnapshot &out) const;
	// Per-frame visual snapshot of item-modeled throwables: tracer-cadence flying
	// rounds with a TrcrID model plus placed devices; the enemy-team item swap
	// follows the viewer team (engine: runtime/world/round_sim.h).
	// The native form is the throwable pass's; the bound form wraps the same
	// rows for the tests.
    void advance_facial_presentation(const Vector3 &p_camera);
	void fill_minefield_draw_rows(std::vector<opennova::world::MinefieldDraw> &r_rows) const;
	void fill_throwable_visual_rows(std::vector<opennova::world::ThrowableVisualRow> &r_rows) const;
	TypedArray<ThrowableVisualRow> get_throwable_visuals() const;
	// Even-tick, simulation-owned watercraft W3/W4 wake samples. The native
	// vector is the fixed-tick presenter's path; the typed array is the
	// diagnostic/test wrapper over the same values.
	void fill_water_wake_frame(
			const Vector3 &camera, opennova::renderer::WaterWakeFrame &frame) const;
	const opennova::particle::ParticleForceField *particle_force_field() const;
	void fill_vehicle_trail_visual_rows(
			std::vector<opennova::world::VehicleTrailVisualRow> &r_rows) const;
	TypedArray<VehicleTrailVisualRow> get_vehicle_trail_visuals() const;

	// The impact-scar draw list for ScarPresenter (simulation_scars.cpp):
	// World::scars compiled through renderer::compile_scar_draws with the shell's
	// camera (Godot space), fog distance and Env_TerrainLightCombined
	// (EnvFile.combine_terrain_light(sun, sky) — the sun+sky combine).
	// { vertices (PackedVector3Array, Godot axes; world space for shared-ring
	//   batches, SECTION-LOCAL for entity-ring batches), uvs, colors,
	//   batch_owner/texture/section/flags(bit0 entity_local, bit1 building)/
	//   first/count, batch_bms_id, batch_spawn_origin, strip_names,
	//   slots_live, slots_culled, rings_leased }. Empty without a world.
	Ref<ScarDrawList> get_scar_draw_list(const Vector3 &p_camera_godot, float p_fog_distance,
			const Color &p_terrain_light) const;
	// The Scar_RenderCache owner gate over OcclusionWorld's section masks and
	// the entity's blink-box quad (see simulation_scars.cpp).
	bool scar_owner_visible(uint16_t p_owner_packed) const;

	// The round hit-detection reality as a HitboxDebugReport
	// (simulation/hitbox_debug_report.h) — the GUT collision oracle: the nearby entity
	// hit meshes and the posed person section spheres.
	// Triangles use the SAME husk-aware target_view + full-euler matrices the
	// projectile raycast uses — the drawn mesh IS the tested mesh; capped at 96
	// entities / 24000 item faces within 80 u of the local player (face_total
	// exposes truncation). Organic posed/fallback spheres share the range/actor
	// cap, omit the local avatar, and use the exact CollisionWorld target
	// matrices consumed by RoundSim.
	Ref<HitboxDebugReport> get_hitbox_debug();

	// The F3 entity picker: one plain geometric trace_projectile segment
	// (terrain / water / static + dynamic CFAC / person bone spheres, nearest
	// wins) along a camera or crosshair ray. Read-only. A DebugPickCard
	// (simulation/debug_pick_card.h) with every field at its typed default;
	// hit=false with blocked = "terrain"/"water"/"proxy" naming why the ray
	// stopped without a pickable entity (proxies = wire geometry).
	Ref<DebugPickCard> debug_pick_entity(const Vector3 &p_from_godot,
			const Vector3 &p_dir_godot, float p_max_range_units);
	// Diagnostic round injector: spawns one live round through the REAL
	// RoundSim::spawn (production velocity/tracer/trail path; owner = the
	// local player) from a Godot-space origin along a Godot-space direction,
	// firing the named ammo ("AMMO_556", ...). The world tick flies it and the
	// F3 Rounds ring records the outcome — the pose-replay probe's seam.
	// Returns the round slot, -1 on bad ammo/full pool.
	int debug_spawn_round(const Vector3 &p_from_godot, const Vector3 &p_dir_godot,
	                      const String &p_ammo_name);

	// Local-player blink state (engine: runtime/world/collision.h). The render/audio hosts gate interior behavior on these.
	bool local_player_indoors() const;
	int local_player_blink_flags() const;
	// items.def id of the pool-2 building encoded by blink_hits[0], or 0 when
	// the player is not inside a blink volume. Entity::item_id is the raw BMS
	// type, so this accessor applies mission::kItemIdOffset for database lookup.
	// Lighting keys from hit PRESENCE, independently of the aggregate "indoors" flag bit.
	int local_player_interior_item_id() const;

	// The blink-box owner for a model-light spawn at a world point: retail runs
	// ONE point query at the spawning entity's position before walking its LGHT
	// records, and slot 0's packed hit names the containing building + section
	// every unattached record binds to (engine: runtime/renderer/light_scene.h). Returns
	// [containing bms_id, section], or an empty array when the point sits in no
	// blink volume (or the containing entity carries no bms identity). The
	// caller applies retail's ItemDef-type gate: a BUILDING never runs the
	// query at all (engine: runtime/renderer/light_scene.cpp).
	PackedInt64Array query_blink_owner_at(const Vector3 &p_world);

	// The per-drawn-entity interior light group: every placed entity currently
	// standing inside a blink volume, as [bms_id, containing bms_id, section]
	// triples. Retail pushes this pair per entity draw so an interior room
	// light reaches exactly the entities in its own section [orig:
	// setup_terrain_effect_for_entity @0x5c74a0 -> Lighting_SetInteriorLightGroup
	// @0x5a90e0, the gate Light_PassesActiveGroups @0x5a9120, see
	// docs/render/render-lighting-re.md]. Entities outside every blink volume
	// are absent (their group is (0, 0)).
	PackedInt64Array get_entity_interior_groups() const;

	// The local player's interior light group: [containing bms_id, section], or
	// an empty array outdoors. The local player is a spawned entity with no
	// bms_id, so it is absent from get_entity_interior_groups; its group is what
	// scopes interior lights onto the first-person arms and weapon.
	PackedInt64Array local_player_interior_group() const;

	// Sound-occlusion distance inflation for the audio host (engine: runtime/audio/ambient_mixer.h). Positions in Godot
	// world space; distance in/out 16.16.
	int64_t sound_occlusion_distance_q16(const Vector3 &listener_pos,
	                                     const Vector3 &source_pos, int64_t distance_q16,
	                                     int source_bms_id = 0);
	// Authored bms id -> registry handle, rebuilt on the registry's spawn
	// serial (retail's slot carries the entity pointer from registration;
	// this is the lookup that identity stands in for).
	opennova::world::EntityHandle handle_for_bms_id(int p_bms_id) const;
    // Native presentation identity for the existing audio occlusion query.
    int sound_source_bms_id(uint16_t p_handle) const;

	// The marched iris-exposure sampling (D-RLIT-2): three classification codes
	// for env::WeatherCore::set_exposure_from_iris_samples — the camera ray runs
	// 8 units forward, clips against terrain, and samples at the end point and
	// two points marched back toward the camera in thirds. Per sample: a blink
	// hit classifies indoor (-1; -2 when the building carries no interior
	// data), else the outdoor sun level 8 minus one per blocked sun-occlusion
	// ray (three entity-only rays, 200 u toward the light, clip radii
	// -0x2000/-0x5000/-0x8000). Positions/directions in Godot world space.
	// Empty when no world/local player is loaded (the caller falls back to the
	// outdoor sample). All three samples reuse the local player's fixed
	// proximity-candidate slice for blink classification, the nonzero-count sun
	// gate, and both pool-1/pool-2 sun blockers.
	// (engine: formats/env/env_weather.h)
	PackedInt32Array compute_iris_samples(const Vector3 &cam_pos, const Vector3 &cam_forward,
	                                      const Vector3 &light_dir);

	// Loadout-zone gates for the host's armory key (engine: formats/def/def.h).
	bool local_player_in_armory_zone() const;
	// The USE key's vehicle-loadout arm (engine: runtime/world/local_player_view.h):
	// the unmounted local player's type-11 volume touch, and the bay's team gate
	// (the ground entity's team 0 or the player's own; a free-standing player
	// reads team 0 = open). False without a kernel.
	bool local_player_in_vehicle_loadout_zone() const;
	bool local_player_vehicle_zone_team_matches() const;

	// The USE-ITEM mount toggle: weapon-busy gate + the witnessed toggle
	// (deck best-seat / nearest-seat scan / seat-swap-or-detach). Returns true when a
	// mount, swap or dismount applied. (engine: runtime/mission/mission_kernel.h)
	bool local_player_toggle_mount();
	bool local_player_select_seat(int p_index);

	// The floating attach labels around the local player (world::AttachLabel:
	// the mission-space position with the +0.1875 u lift applied, the seat
	// type (world::SeatType, 4 = armory point), the armory-zone flag, the
	// nearest full-bright highlight and the USEGUN weapon's attachtextid
	// Overlays key, "" = absent -> the STROVER_USEGUN default). Armory mode
	// rides the zone flag; the nearest-only gate consumes the same complete
	// live fire verdict as body/HUD selection. NOT ClassDB-bound:
	// HudOverlay::set_attach_labels projects and resolves the rows natively.
	// False without a kernel. (engine: runtime/hud/hud_frame.cpp)
	bool fill_attach_labels(std::vector<opennova::world::AttachLabel> &r_labels) const;
	// The friendly-tags gather (D-HUD-20; world::FriendlyTagSource): the raw
	// positions + per-entity facts; HudOverlay::set_friendly_tags lifts,
	// projects and feeds the compiler's element natively. NOT ClassDB-bound.
	// False without a kernel or a local player.
	bool fill_friendly_tags(std::vector<opennova::world::FriendlyTagSource> &r_tags) const;
	// The radio-request icon's viewer gate over the local player (world::
	// friendly_tag_radio_request_viewer): a driver/controller seat or an own latch.
	bool local_player_radio_request_icon_viewer() const;

	// Parse weapon.def from the resource root and install the armory table on the sim world
	// (world::World::weapons) — the server-side source for the 0x2F/0x5A loadout service, the
	// extended-uplink equipped-weapon gate, and the player-spawn WPN_M4AUTO default
	// (D-NET-141/143). [orig: Game_StartMission @0x5254bd -> WeaponDefs_LoadFile @0x5450A0,
	// right after AnimDef_InitAll @0x5254b3]. Idempotent; call after load.
	Error load_weapon_table(const Ref<class ResourceRoot> &p_resource_root,
	                        const String &p_name = "weapon.def");

	// Parse score.ini and install this session's scoring awards (world::World::score_rules).
	// Retail builds 12 x 452-byte gametype rows with hardcoded defaults and then OVERLAYS
	// the file onto them, writing the file out when it is absent
	// (engine: runtime/inmatch/game_config.h).
	// DECLARED GAP: the built-in defaults are NOT ported, so a missing score.ini leaves
	// score_rules !valid (every award a no-op) where retail would still score from its
	// defaults. The shipped file is the retail-parity path.
	// Order-independent with the mission load: whichever of the two lands second
	// re-resolves the row (see refresh_score_rules).
	Error load_score_config(const Ref<class ResourceRoot> &p_resource_root,
	                        const String &p_name = "score.ini");

	// Parse ammo.def and install the ballistics/damage table (world::World::ammo), then
	// resolve every armory entry's round_type to its ammo index — the authoritative round
	// sim's data feed (§5.60). Call after load_weapon_table.
	// (engine: base/gameprofile/required_resources.c)
	Error load_ammo_table(const Ref<class ResourceRoot> &p_resource_root,
	                      const String &p_name = "ammo.def");

	int get_spawned_count() const { return kernel_->promo.spawned; }
	int get_brain_count() const { return kernel_->promo.brains; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::Simulation::PresentField);
VARIANT_ENUM_CAST(godot::Simulation::EffectStateField);
VARIANT_ENUM_CAST(godot::Simulation::SeatCode);
VARIANT_ENUM_CAST(godot::Simulation::MountCommand);
VARIANT_ENUM_CAST(godot::Simulation::JoinTerrainTilState);
VARIANT_ENUM_CAST(godot::Simulation::SessionRole);
VARIANT_ENUM_CAST(godot::Simulation::Stance);
VARIANT_ENUM_CAST(godot::Simulation::WeaponCategory);
