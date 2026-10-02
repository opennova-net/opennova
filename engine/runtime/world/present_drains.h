#pragma once

// The per-tick presentation drain rows the present passes read (ADR 0043
// d10): every row is an engine-owned value in MISSION space (x east, y north,
// z up; the device layer axis-maps on read), the typed twin of the world state
// it summarizes — the fire ring, the resolved impacts, the live rounds and
// placed devices, the death-piece pool, the round glows. The fills below
// (present_drains.cpp) build them from the world; the embedder only gates on a
// live world and forwards. The C++ present passes read the vectors directly;
// the Godot records wrap one row by value.

#include <runtime/world/geom.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

class World;

// One round spawned since the last drain: the EFFECT legs of both retail
// receive arms. `adm_arm` selects the adm-indexed arm, whose `action_effect`
// spawns at the addressed weapon's `action_userpoint` on the rendered gun;
// the ammo arm spawns `effect` at `origin` along `forward` (a unit vector in
// the mission frame: cos yaw * cp, sin yaw * cp, sin pitch).
struct FirePresentationRow {
    Vec3 origin;
    bool adm_arm = false;
    int32_t adm_index = 0;
    Vec3 forward{0.0f, 1.0f, 0.0f};
    int32_t shooter_handle = -1;
    int32_t source_bms_id = 0;
    bool is_local_player = false;
    int32_t ammo_index = 0;
    std::string effect;
    int32_t mf_light = 0;
    std::string action_effect;
    std::string action_userpoint;
};

// One resolved round impact, already mapped through the ammo effects_table
// to its effect and sound legs. `age_ticks` is the catch-up pre-age;
// `has_light` is the light_impact gate (the effect leg AND an authored
// radius), with the flash fields meaningful only when set.
struct RoundImpactPresentation {
    Vec3 position;
    Vec3 direction;
    std::string effect;
    // The impact's selected soundset, for observers only: the engine plays it
    // where the impact is produced (play_round_impact_sound), never the shell.
    std::string sound;
    uint32_t age_ticks = 0;
    uint32_t source_tick = 0;
    uint64_t source_order = 0;
    bool section_tagged = false; // RoundImpact::section_tagged
    bool has_light = false;
    float light_radius = 0.0f;
    uint32_t light_color_rgb24 = 0xFFFFFFu;
    int32_t light_ticks = 10;
};

// One item-modeled throwable: a tracer-cadence flying round with a TrcrID
// model or a placed device. `key` is generation<<10 | pool slot for rounds
// and 0x40.. | entity for devices, so a same-slot replacement never inherits
// the outgoing model or effect group; the eulers are the placer convention
// (pitch, MISSION yaw, roll) in degrees. `move_effect` is the effects_table
// tag-1 round-bound particle and `move_effect_live` its emitter liveness (the
// round+0x1CC handle mirror).
struct ThrowableVisualRow {
    int64_t key = 0;
    int32_t item_id = 0;
    Vec3 pos;
    float pitch_deg = 0.0f;
    float yaw_deg = 0.0f;
    float roll_deg = 0.0f;
    std::string move_effect;
    bool move_effect_live = true;
};

// One sampled watercraft wake frame. `handle_packed` plus the registry spawn
// generation identifies one vehicle lifetime; bms/spawn_origin let the shell
// prefer its placed model and the packed handle resolves a wire-built model.
// The pose, water plane, and both Q16 controls come from the SAME even-tick
// cbot sample. W3 is commanded speed, W4 current signed motion.
struct VehicleTrailVisualRow {
	int32_t handle_packed = -1;
	uint64_t registry_spawn_id = 0;
	uint8_t point = 0;
	uint32_t source_tick = 0;
	Vec3 pos;
	Vec3 dir;
	std::string effect;
	uint32_t magnitude_q16 = 0;
	// Every trail lane spawns with the vehicle as its descriptor tag, so the
	// group takes the building-section gate [orig: Entity_UpdateBoneTrailEffects
	// @ 0x4589C0 — the entity in ebp @ 0x4589C8, stored at descriptor +0x0C
	// @ 0x458C5F, the spawn into the lane's handle @ 0x458D55].
	bool section_tagged = true;
};

// One live death piece: the pool slot and its allocation generation (a
// same-slot reuse is a new piece), the husk model source and the ONE section
// it renders, the debris-type row (death_piece_trail_effect names its trail),
// the render scale, the position, the integrated heading / pitch in degrees,
// and the settled (persistent ground debris) bit.
struct DeathPieceRow {
    int32_t slot = -1;
    uint64_t generation = 0;
    int32_t item_id = 0;
    int32_t section = 0;
    int32_t type_index = 0;
    float scale = 1.0f;
    Vec3 pos;
    float heading = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    bool settled = false;
    // False for a silent death's piece: no trail is ever submitted
    // [orig: Entity_SpawnDeathPieces @0x493811].
    bool trail = true;
};

// One in-flight round glow: the round's presentation generation as the id,
// its position and the ammo's light_move radius / packed 0x00RRGGBB color.
struct RoundGlowRow {
    uint64_t id = 0;
    Vec3 pos;
    float radius = 0.0f;
    uint32_t color_rgb24 = 0xFFFFFFu;
};

// The fills. Each clears `r_rows` first; the two drains also clear the world
// ring they consumed (the fire ring, the resolved impacts), so a second call
// on the same tick yields nothing.
void fill_throwable_visual_rows(const World &world, std::vector<ThrowableVisualRow> &r_rows);
void fill_vehicle_trail_visual_rows(const World &world, std::vector<VehicleTrailVisualRow> &r_rows);
void drain_round_impact_rows(World &world, std::vector<RoundImpactPresentation> &r_rows);
void drain_fire_presentation_rows(World &world, std::vector<FirePresentationRow> &r_rows);
void fill_death_pieces(const World &world, std::vector<DeathPieceRow> &r_pieces);
void fill_round_glows(const World &world, std::vector<RoundGlowRow> &r_rows);

} // namespace opennova::world
