// Class-body fragment of godot::Simulation — #include'd exactly once by
// simulation/simulation.h INSIDE the class, at public: access. Not a
// standalone header: no include guard on purpose, so a second inclusion
// fails loudly with redefinition errors. New bound enums/constants belong
// here, not in the main header.
//
// The bound enum/constant surface: the GDScript-visible class-scope enums
// (BIND_ENUM_CONSTANT needs class scope) re-exporting the engine's values,
// plus the spawn-origin pack/unpack helpers.
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
		PF_STANCE_BITS = opennova::world::PF_STANCE_BITS,
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
