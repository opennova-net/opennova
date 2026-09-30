// The local player's radar contact state and its producers: the 128-row blip
// table the damage and tracer-whiz legs fill, its per-HUD-frame sector
// rebuild into the four red/olive 12/24 rings, the four compass-edge timers,
// and the incoming guided-missile list. The spinmap's content-mask bit 10
// draws what this state holds (hud/hud_minimap_radar.cpp); the HUD reads it
// through a once-per-frame snapshot (radar_hud_frame -> hud::HudMinimapRadar).
// [orig: Radar_AddBlip @0x59b280; Radar_UpdateContacts @0x59a7e0;
//  sub_59B200 @0x59b200 (the missile list); HUD_ResetAllOverlayBuffers
//  @0x59dd40 (the reset); the state @0x2721EEC..0x2721F3C, @0x2721F40,
//  @0x2722B40, @0x2723EAC]
#pragma once

#include <array>
#include <cstdint>

#include <runtime/world/entity.h>

namespace opennova::audio {
class SoundSetIndex;
}
namespace opennova::hud {
struct HudMinimapRadar;
}

namespace opennova::world {

class World;
struct LiveRound;
struct FixedVec3;
struct AmmoTableEntry;
struct AmmoTable;

// [orig: the 128 x 24-byte table @0x2721F40, walked to 128 @0x59b306 /
//  @0x59a990; the 64 x 16-byte missile list @0x2722B40, capped @0x59b231]
inline constexpr int kRadarContactRows = 128;
inline constexpr int kRadarMissileRows = 64;
// A fresh row's life and a fresh edge timer, in HUD ticks
// [orig: slot[4] = 62 @0x59b327; the edge word = 31 @0x59b2e3].
inline constexpr int32_t kRadarContactLife = 62;
inline constexpr uint16_t kRadarEdgeTimer = 31;

// The row's kind word (+0x14) picks the ring its bearing lights
// [orig: Radar_UpdateContacts @0x59a8d4..0x59a97f]: 0 the red 12-ring, 1 the
// olive 12-ring, 2 the red 24-ring, 3 the olive 24-ring, 255 every red
// 12-sector from the row's loop index on (the self-damage "all around").
inline constexpr int32_t kRadarKindRed12 = 0;
inline constexpr int32_t kRadarKindOlive12 = 1;
inline constexpr int32_t kRadarKindRed24 = 2;
inline constexpr int32_t kRadarKindOlive24 = 3;
inline constexpr int32_t kRadarKindSelf = 255;

// The mpattrib bit that gates the whole system: NoTracers ("Tracers 0")
// [orig: g_RulesFlags @0x24D1E34 & 1 — Radar_AddBlip @0x59b283, the dmgslice
//  drawer @0x59c371, the ring's red-12 blink @0x5982e6; the only writer
//  CAdminServer_HandleSetCommand @0x405f58..0x405f86].
inline constexpr uint32_t kRadarRulesNoTracers = 0x1u;

struct RadarContact {
	uint32_t source = 0; // +0x00: the producing entity; stored, never read
	int32_t pos[3] = {}; // +0x04..+0x0C, mission 16.16
	int32_t life = 0;    // +0x10: HUD ticks left, 0 = a free row
	int32_t kind = 0;    // +0x14
};

struct RadarMissile {
	uint32_t source = 0; // +0x00: the missile; 0 is the ring's null test
	int32_t pos[3] = {}; // +0x04..+0x0C, copied at note time; the ring reads
	                     // the missile's LIVE position through +0x00 instead
};

struct RadarContactState {
	std::array<RadarContact, kRadarContactRows> rows{};
	// Four compass-edge timers [orig: dword_2721EEC as u16[4]]: stamped by
	// every add, decayed by every update, drawn by nothing — their drawer
	// HUD_DrawDamageDirectionIndicators @0x59a300 has no caller in JO.
	std::array<uint16_t, 4> edge{};
	// The sector bytes, rebuilt from zero by every update
	// [orig: red12 dword_2721F30, olive12 dword_2721F24, red24 dword_2721F0C,
	//  olive24 dword_2721EF4; zeroed @0x59a80a..0x59a85f].
	std::array<uint8_t, 12> red12{};
	std::array<uint8_t, 12> olive12{};
	std::array<uint8_t, 24> red24{};
	std::array<uint8_t, 24> olive24{};
	uint32_t last_tick = 0; // [orig: dword_2723EAC]
	// The incoming guided-missile list. The append writes ONE PAST the count
	// (index count + 1), so row 0 stays null and the newest row sits outside
	// the [0, count) range every reader walks; the 64th append writes past the
	// 64-row table into its neighbour, which the extra row stands for
	// (nothing here reads it) [orig: sub_59B200 @0x59b236..0x59b258].
	std::array<RadarMissile, kRadarMissileRows + 1> missiles{};
	int32_t missile_count = 0; // [orig: dword_2721F3C]
};

// [orig: Radar_AddBlip @0x59b280] — `local_pos`/`local_yaw` are
// g_LocalPlayerEntity's Position and Yaw; `rules` the mpattrib word.
void radar_add_blip(RadarContactState &state, uint32_t rules, const int32_t local_pos[3],
		uint32_t local_yaw, uint32_t source, const int32_t pos[3], int32_t kind);
// [orig: Radar_UpdateContacts @0x59a7e0] — `viewer_pos`/`viewer_yaw` are the
// HUD viewer's (g_HUDInfoCurrentEntity) Position and Yaw. The trailing
// MapOverlay_UpdateTimers(d) call @0x59a9ce is not made here: the retained
// map banks age on their own clock in this runtime (the replica pipeline).
void radar_update_contacts(RadarContactState &state, uint32_t tick, const int32_t viewer_pos[3],
		uint32_t viewer_yaw);
// [orig: sub_59B200 @0x59b200]
void radar_note_missile(RadarContactState &state, uint32_t source, const int32_t pos[3]);
// [orig: HUD_ResetAllOverlayBuffers @0x59dd40 — the missile rows @0x59dd49,
//  the contact rows @0x59dd59, the edge timers @0x59dd65/@0x59dd6a, the tick
//  @0x59dd83; the missile COUNT is not touched]
void radar_reset(RadarContactState &state);

// The source Player_OnDamageReceived / Radar_AddBlip receive: an entity
// pointer (null for none) whose +0x170 word — a projectile's shooter, a
// vehicle's first occupant, a person's ride link (Entity::primary_occupant) —
// the damage kind rule reads. `id` is the identity the row stores (+0x00).
struct RadarSource {
	uint32_t id = 0;              // 0 = the null source
	bool local = false;           // source == g_LocalPlayerEntity
	const Entity *link = nullptr; // *(source + 0x170)
};
// An entity source (its +0x170 = Entity::primary_occupant).
RadarSource radar_entity_source(const World &world, EntityHandle source);
// A round source: the projectile's +0x170 is its shooter (LiveRound::owner).
RadarSource radar_round_source(const World &world, const LiveRound &round, uint32_t slot);
// Self damage is 255; otherwise 2 when the source's +0x170 entity is class 6,
// else 0 [orig: Player_OnDamageReceived @0x4dd8b3..0x4dd8e4].
int32_t radar_damage_kind(const RadarSource &source);

// Radar_AddBlip as the world producers call it: the local player's pose and
// the session mpattrib word; a no-op without a bound local player body.
void radar_add_blip(World &world, uint32_t source, const int32_t pos[3], int32_t kind);

// The tracer whiz: a round whose segment passes within the ammo's whiz
// radius of the listener lights its shooter's bearing on the olive rings,
// once per round. Called at the tail of the round's ballistic tick with the
// tick-start position, the tick's end point and the incoming velocity.
// [orig: Projectile_UpdatePhysics @0x4ea98e..0x4ea9f2 ->
//  Projectile_SpawnTracerScarEffect @0x4e5ac0]
void round_tracer_whiz(World &world, LiveRound &round, const AmmoTableEntry *ammo,
		const FixedVec3 &start, int32_t end_x, int32_t end_y, const FixedVec3 &velocity);
// Stamp every ammo's whiz radius (AmmoTableEntry::whiz_radius_q16, ammo
// +0x8C) from the loaded sound sets; a null index leaves every radius 0.
// [orig: AmmoDef_InitEffectsTable @0x40a04b..0x40a07f]
void resolve_ammo_whiz_radii(AmmoTable &ammo, const audio::SoundSetIndex *sets);
// The Stinger motor's lock note: a missile whose target is the local player,
// or a vehicle the local player is the first occupant of, rides the missile
// list [orig: Entity_UpdateGuidedMissile_0 @0x4465db..0x446622 -> sub_59B200].
// The lock-tone word the same site sets (dword_B764C0 = 31 @0x4465f8 /
// @0x446618) has no consumer here: its reader registers the unported
// SndLpLockOnMe loop [orig: HUD_RenderAllOverlays @0x5a8197..0x5a81bf].
void radar_note_guided_missile(World &world, const LiveRound &round, uint32_t slot);

// One HUD frame of the radar legs in HUD_RenderAllOverlays order, into the
// snapshot the spinmap compile draws: the contact update behind the pass's
// early-outs (`pass_runs`: no spawn-success gate, hud_detail < 3; the white
// flash and a missing local player are read here) and the death-screen gate,
// which the map site's own call (`map_site`: the corner map draws with bits
// 9/6/10) reopens; then the snapshot; then the per-frame missile-count clear.
// [orig: HUD_RenderAllOverlays early-outs @0x5a8084..0x5a80db, the update
//  @0x5a8164..0x5a817d, HUD_DrawMapOverlay's site @0x5a78ff..0x5a791c, the
//  count clear @0x5a87ef]
void radar_hud_frame(World &world, uint32_t tick, bool pass_runs, bool map_site,
		hud::HudMinimapRadar &out);

} // namespace opennova::world
