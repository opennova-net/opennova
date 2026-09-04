#pragma once

// The per-tick presentation drain rows the embedder's Simulation fills from
// the world's outputs for its present passes (ADR 0043 d10): every row is an
// engine-owned value in MISSION space (x east, y north, z up; the device layer
// axis-maps on read), the typed twin of the world state it summarizes — the
// fire ring, the resolved impacts, the live rounds and placed devices, the
// death-piece pool, the round glows. The C++ present passes read the vectors
// directly; the Godot records wrap one row by value. The witnesses live on
// the fills (Simulation::drain_* / fill_*) and on the world state each reads.

#include <runtime/world/geom.h>

#include <cstdint>
#include <string>

namespace opennova::world {

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
    std::string sound;
    uint32_t age_ticks = 0;
    uint32_t source_tick = 0;
    uint64_t source_order = 0;
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
struct VehicleWakeVisualRow {
    int32_t handle_packed = -1;
    uint64_t registry_spawn_id = 0;
    int32_t item_id = 0;
    int32_t bms_id = 0;
    uint32_t spawn_origin = 0xFFFFFFFFu;
    uint32_t source_tick = 0;
    Vec3 pos;
    float pitch_deg = 0.0f;
    float yaw_deg = 0.0f;
    float roll_deg = 0.0f;
    int32_t water_z = 0;
    bool afloat = false;
    std::string w3_effect;
    std::string w3_userpoint;
    uint32_t w3_magnitude_q16 = 0;
    std::string w4_effect;
    std::string w4_userpoint;
    uint32_t w4_magnitude_q16 = 0;
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
    bool settled = false;
};

// One in-flight round glow: the round's presentation generation as the id,
// its position and the ammo's light_move radius / packed 0x00RRGGBB color.
struct RoundGlowRow {
    uint64_t id = 0;
    Vec3 pos;
    float radius = 0.0f;
    uint32_t color_rgb24 = 0xFFFFFFu;
};

} // namespace opennova::world
