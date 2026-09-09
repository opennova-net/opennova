// The packed present-row contract: field layout of one entity record in the
// per-tick present snapshot's flat float buffer. ONE batched float-array read
// replaces per-entity scalar getters in the present loop (the scalar getters
// box a value each; see feedback_dispatcher_callable_perf). The engine owns
// this layout; the shell binding re-exports it as bound constants (the
// GDExtension enum on the simulation binding) so scripts and C++ share a
// single source of truth. Rotation is emitted as mission-space degrees
// (pitch, yaw, roll) so the shell builds the basis through the one placer
// convention; position is already in present space (mission (x,y,z) 16.16 ->
// (x, z, -y) units). Infantry remains yaw-only; vehicle rows publish their
// live attitude.
#pragma once

namespace opennova::world {

enum PresentField : int {
	PF_KIND = 0, // mission ItemType (3 = Organic), -1 if none
	PF_INDEX, // index within its kind's list
	PF_BMS_ID, // file entity id; the shell maps this to a placed node (primary key)
	PF_NET_ID, // runtime SSN (WAC/BMS addressing)
	PF_POS_X, // present-space position (mission (x,y,z) 16.16 -> (x, z, -y) units)
	PF_POS_Y,
	PF_POS_Z,
	PF_PITCH_DEG, // mission-space rotation, degrees (live Entity, decoded/predicted
				  // client vehicle, or the client attachment pose)
	PF_YAW_DEG,
	PF_ROLL_DEG, // same pose source as PF_PITCH_DEG
	PF_PHASE1, // channel 1 signed dword low16, exact as numeric float
	PF_ACTIVE1, // 0 unpublished; otherwise high16+1 (FastRope may suppress)
	PF_PHASE2, // channel 2 signed dword low16
	PF_ACTIVE2, // 0 unpublished; otherwise high16+1
	PF_BODY_ANIM_SLOT, // Entity.body_anim_slot (main-body .bad/.adm clip; consumed only by the
					   // deferred seam)
	PF_ANIM_STATE, // InfantryState.anim_state (full off_8135F0 state id; -1 when unavailable)
	PF_ANIM_PHASE_TICKS, // body-clip phase in IDA half-frame ticks; -1 when the compact omits it
	// The authoritative outgoing PRIMARY channel and exact float32 target
	// weight. Host/NPC rows carry the live AnimMap tuple; remote-request rows
	// leave source=-1/weight=1 and reconstruct it at the receive-side FSM.
	PF_ANIM_SOURCE_STATE,
	PF_ANIM_SOURCE_PHASE_TICKS,
	PF_ANIM_BLEND_WEIGHT,
	PF_ANIM_REMOTE_REQUEST, // 1 = compact request needs receive-side arbitration; 0 = authoritative
							// current state
	// A transition state observed and then OVERWRITTEN within one decode fold
	// (several 0x0A datagrams can apply between present drains). Since
	// D-NET-209 the per-record receive arbitration [orig: @0x4c1153] runs
	// natively in the fold and ARMED rows publish the arbitrated channel
	// (remote_request 0); the pulse remains the DISARMED-row fallback (a
	// TAPPED prone roll on a dead/carried/adm-less row).
	// Presentation dispatches the pulse BEFORE the current state so the
	// model's arbitration replays retail's per-record order.
	// -1 = none; MUST stay ahead of PF_AIM_OVERLAY_VALID (zero-fill would read
	// as valid state 0 = anim_reset).
	PF_ANIM_STATE_PULSE,
	PF_ANIM_PULSE_TICKS,
	// The SECONDARY (upper-body weapon) channel — the hold-pose ladder every
	// observer re-derives for every player body, local or remote. -1 = no channel
	// this frame. These MUST stay ahead of PF_AIM_OVERLAY_VALID: rows are seeded
	// only up to that point, and anim state 0 is a valid key (anim_reset), so a
	// zero-filled weapon state would splice the reset clip over every arm.
	PF_WPN_ANIM_STATE,
	PF_WPN_PHASE_TICKS, // secondary clip phase in IDA half-frame ticks
	// The secondary channel's cross-fade + served variants, mirroring the primary's
	// PF_ANIM_SOURCE_* trio: outgoing state/phase (-1 = not blending), the ramping
	// weight (1 = settled), and the ring entries latched for target and outgoing.
	// Same seeding rule as above — ahead of PF_AIM_OVERLAY_VALID.
	// [orig: AnimMap_UpdateEntity @0x40b5f0 re-init; the +68 play latch]
	PF_WPN_SOURCE_STATE,
	PF_WPN_SOURCE_PHASE_TICKS,
	PF_WPN_BLEND_WEIGHT,
	PF_WPN_VARIANT,
	PF_WPN_SOURCE_VARIANT,
	PF_HIDDEN, // 1 when the entity is hidden
	// Local render-only verdict: skip this placed entity's own world model.
	// Does not mutate Entity.hidden, collision, simulation, or attached actors.
	PF_LOCAL_VIEW_SUPPRESSED,
	PF_ALIVE, // 1 when alive
	PF_RESPAWN_REVISION, // decoded organic dead->alive epoch; resets remote body state
	PF_TYPE_ID, // items.def runtime type id from the wire (0 = none); keys the joiner's wire
				// avatars
	PF_WIRE_HANDLE, // (pool<<12)|slot wire handle; zero is a valid pool-0 identity
	// The packed character id (entity+0x15C: nationality|division|combo|side) the
	// 0x0C spawn echoes for a player; 0 when not a player. Keys the composed
	// head/body the joiner presents for that row.
	PF_CHARACTER_ID,
	// The decoded wire carrier link, -1 for none: for a standing player this
	// is the groundEntity it stands on (entity+0x28) — the same link the
	// authority body channel takes from Entity::ground_target for the
	// on-entity footstep slot [orig: the +0x28 store by
	// Entity_RaycastGroundHeightAndObject @0x525fd0]. For a SEATED body the
	// wire reuses the field for the seat carrier (mount_bone distinguishes);
	// seat clips author no foot-event bits, so the footstep consumer never
	// reads it in that state.
	PF_CARRIER_HANDLE,
	// Final output of anim::compute_aim_overlay_angles. Presentation consumes
	// this result; it never repeats the mounted config selector.
	PF_AIM_OVERLAY_VALID,
	PF_AIM_BODY_PITCH_DEG,
	PF_AIM_BODY_YAW_DEG,
	PF_AIM_BODY_ROLL_DEG,
	PF_AIM_ANGLES, // nine contiguous (pitch,yaw,roll) triples, OverlayClass order
	PF_AIM_CLASS_STRIDE = 3,
	// Semantic emplaced-weapon PANM registers. These are deliberately not
	// PF_PHASE1/2: PLAYPARTANIM publishes those on VEHICLE_SPECIAL1/2.
	PF_EMPLACED_CONTROLS_VALID = PF_AIM_ANGLES + 9 * PF_AIM_CLASS_STRIDE,
	PF_EWEAP_GUNYAW,
	PF_EWEAP_GUNPITCH,
	// Vehicle render controls projected from authoritative motor/brain state.
	// CTRL_MASK selects the callback-owned groups. Joiner compacts lack the source fields, so those
	// rows remain invalid rather than inferring motion from lossy transforms.
	// [orig: Entity_CacheVehicleHUDStats @ 0x4929B0;
	//  VEHICLE_STEERING @ 0x4929C0..0x4929D7;
	//  VEHICLE_SPEED @ 0x4929DC..0x4929F1]
	PF_VEHICLE_MOTION_VALID,
	PF_VEHICLE_CTRL_MASK,
	PF_VEHICLE_TRACK_LEFT,
	PF_VEHICLE_TRACK_RIGHT,
	PF_VEHICLE_GUN_YAW,
	PF_VEHICLE_GUN_PITCH,

	PF_VEHICLE_STEERING,
	PF_VEHICLE_SPEED,
	// The part-animation registers the same callback publishes: the rotor
	// angle accumulator's high word (HELO_ROTOR / HELO_TAILROTOR) and the
	// wheel phase's (VEHICLE_WHEELS). Valid with PF_VEHICLE_MOTION_VALID.
	// [orig: Entity_CacheVehicleHUDStats @0x4929B0; the +0x466 read
	//  @0x492ACA..0x492ADE; the +0x2BA read @0x4929B4; the accumulators are
	//  owned by Entity_UpdatePartSpinAccumulator @0x4928B0]
	PF_VEHICLE_ROTOR,
	PF_VEHICLE_TAIL_ROTOR,
	PF_VEHICLE_WHEELS,
	PF_VEHICLE_TIRE00,
	PF_VEHICLE_TIRE01,
	PF_VEHICLE_TIRE02,
	PF_VEHICLE_TIRE03,
	PF_VEHICLE_TIRE04,
	PF_VEHICLE_TIRE05,
	PF_VEHICLE_TIRE06,
	PF_VEHICLE_TIRE07,
	PF_VEHICLE_TIRE08,
	PF_VEHICLE_TIRE09,
	PF_VEHICLE_TIRE10,
	PF_VEHICLE_TIRE11,
	PF_VEHICLE_TIRE12,
	PF_VEHICLE_TIRE13,
	PF_VEHICLE_GEAR,
	// Retail CTRL writers around a rendered world model. TEX_TEAM is written
	// for every sector-model submission and again by the generic callback for
	// numbered zones. TEAMSWING is owned by that zone callback. LFP is a
	// conditional write: the packed zone byte and a client timer-list entry
	// must both exist, so its own validity bit cannot be collapsed into
	// PF_ZONE_CTRL_VALID.
	// [orig: render_sector_entity @0x5C424F..0x5C425F;
	//  BoneCallback_gnrc_World @0x4E288B..0x4E28FB]
	PF_TEX_TEAM_VALID,
	PF_TEX_TEAM,
	PF_ZONE_CTRL_VALID,
	PF_TEAMSWING,
	PF_LFP_CAMPPERCENT_VALID,
	PF_LFP_CAMPPERCENT,
	// Attachment-scoped carrier HEAT_GLOW. VALID means this carrier owns a
	// live UseGun child/bone relation; that scope publishes cold zero too.
	// Other authoritative entities and joiner compact rows leave it invalid.
	// The value saturates at 0xFFFF; the separate FP path reaches 0x10000.
	// [orig: attachment call @ 0x546518;
	//  HUD_CacheWeaponSlotInfo @ 0x44095B..0x440991]
	PF_WORLD_HEAT_GLOW_VALID,
	PF_WORLD_HEAT_GLOW,
	// Retail's derived skeletal clipping verdict. For a non-player organic in
	// controller/gunner/driver (never passenger), presentation zero-scales
	// BN17 at its animated joint while collision emits its literal zero row.
	PF_RIGHT_HAND_COLLAPSED,
	// The THIRD-PERSON held weapon: which ADM model this body is holding, and the
	// weapon's own attach orientation (mission euler degrees). These sit in the
	// zero-filled tail deliberately: a default ADM of 0 means DRAW NOTHING, which is
	// both our weapon table's null row and the original's own precondition
	// [orig: `if (entity->equippedAdmIndex)` @ 0x4e3c97]. The draw gate is folded in
	// here rather than carried separately — a hidden weapon simply reports 0.
	PF_HELD_WEAPON_ADM,
	PF_HELD_WEAPON_PITCH_DEG,
	PF_HELD_WEAPON_YAW_DEG,
	PF_HELD_WEAPON_ROLL_DEG,
	// Which of the original's TWO attach frames this body's weapon takes. Retail
	// picks between them on one bit of the WEAPON-channel hold state:
	// `g_animStateFlagsTable[entity+0x2C8] & 0x80` selects the hand-oriented frame
	// (bone 16's matrix with a fixed calibration) instead of the entity angle triple
	// [orig: gate @ 0x4b21b6, branch @ 0x4b220f]. Bit 0x80 is set for the knife,
	// grenade and designator holds, both melee attacks, binoculars, BOTH reload
	// states, and the death family — so this is an ordinary-play path, not an edge
	// case. Zero (the default) means the entity-triple frame, which is what an
	// unarmed or hidden body should report anyway.
	PF_HELD_WEAPON_HAND_FRAME,
	// Entity::section_mask is a hidden-section uint32. Float32 cannot carry it
	// losslessly as one scalar, so publish two exact u16 halves plus ownership.
	// Presentation reconstructs it and sends ~hidden to ObjectModel's visible
	// section mask. Compact LAN rows leave VALID clear; this is host/SP state.
	PF_SECTION_MASK_VALID,
	PF_SECTION_MASK_LO,
	PF_SECTION_MASK_HI,
	// Retail's MoveOrder crouch/prone bits (entity+300 & 0x300), normalized
	// to bit0=prone, bit1=crouched for MATCHTERRAIN presentation. This tail
	// field is zero-filled for non-organics and unsupported compact rows.
	PF_STANCE_BITS,

	// Focal-wind second-bone deformation supplied by the Sway renderer.
	PF_FOCAL_SWAY_VALID,
	PF_FOCAL_SWAY_BASIS_0,
	PF_FOCAL_SWAY_BASIS_1,
	PF_FOCAL_SWAY_BASIS_2,
	PF_FOCAL_SWAY_BASIS_3,
	PF_FOCAL_SWAY_BASIS_4,
	PF_FOCAL_SWAY_BASIS_5,
	PF_FOCAL_SWAY_BASIS_6,
	PF_FOCAL_SWAY_BASIS_7,
	PF_FOCAL_SWAY_BASIS_8,
	PF_FOCAL_SWAY_X,
	PF_FOCAL_SWAY_Y,
	PF_FOCAL_SWAY_Z,
	// How many door phases this row publishes (0 for every row without door
	// motion; the presenter's ownership/release edge). The phases themselves
	// ride the door side table beside the rows (runtime/inmatch/present_rows.h,
	// exact signed dwords including phase 65536, following retail's ordinal
	// bus beyond DOOR_15 when authored), so only door-bearing rows pay for it.
	PF_DOOR_COUNT,
	PF_STRIDE
};

} // namespace opennova::world
