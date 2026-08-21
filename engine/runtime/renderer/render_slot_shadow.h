// The render-slot entity ground-shadow planner — the portable half of the
// retail per-entity shadow pipeline (docs/render/render-lighting-re.md,
// "the render-slot (entity ground shadow) side").
//
// Retail shape: a 256-record slot table (128 B each)
// [orig: RenderSlot_Table @ 0x2be3d30] registered at entity init — persons
// always, items via the attrib2 DynamicShadow bit, gated on the shadow-detail
// option [orig: Entity_InitFromModel @ 0x40E1C8..0x40E1F7 ->
// shadow_decal_alloc_slot @ 0x5d5690]. Each frame the slot pass
// [orig: render_shadow_pass @ 0x5d7b70]:
//   1. loads the sun into the slot default direction, clamps the vertical
//      component to 0.25 and negates all three (light->surface form; a
//      grazing sun never stretches a silhouette past 4x height);
//   2. scores and sorts every slot by camera distance x view alignment,
//      binds the best 24 to drape patches and the first 12 to silhouette
//      render targets [orig: terrain_sort_and_assign_render_slots
//      @ 0x5d6530; RT chain init_render_target_chain @ 0x5d5320];
//   3. per slot picks the dominant nearby point light (NTSC luminance over
//      distance^2 attenuation; an interior-parented entity zeroes the win
//      threshold) and marches the shadow anchor from the entity along the
//      light direction down to terrain [orig: RenderSlot_UpdateEntityLight
//      @ 0x5d6a30, height probe Terrain_GetHeightAtPosition @ 0x606720];
//   4. renders each live silhouette RT on the detail-scaled refresh cadence
//      [orig: RenderSlot_RenderEntityAndChildren @ 0x5d7690] with the slot
//      lighting constants [orig: RenderSlot_SetupNextLighting @ 0x5d7250];
//   5. drapes each bound slot over a 21x21 terrain-following patch: the
//      silhouette projected along the slot direction and multiplied into the
//      terrain with the per-channel ambient law and the 40..80 u distance
//      fade — or, for a bound slot without a silhouette RT, the authored
//      items.def `shadow` blob decal, heading-rotated
//      [orig: RenderSlot_DrawAllDrapes @ 0x5d6e20 -> render_sector_model
//      (drape) @ 0x5d5ca0 / render_minimap_tile_overlay (authored blob)
//      @ 0x5d59d0].
//
// This unit carries every planning/selection/color law as a structural
// translation; the device half (godot/src) realizes the silhouette capture
// and the terrain drape. The retail anchor march is the patch PLACEMENT for
// the drape mesh — a per-pixel projective drape computes the same terrain
// intersection the march approximates (its vertical step is clamped to
// >= 0.5 u per planar unit), so a projective device realizes the march's
// observable exactly; the march law is still ported here for parity tests.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace renderer {

// ---------------------------------------------------------------------------
// Direction and geometry laws
// ---------------------------------------------------------------------------

// The frame-open slot projection direction: vertical component clamped to
// >= 0.25, then all three components negated into the light->surface form
// [orig: render_shadow_pass @ 0x5d7bdc..0x5d7c30]. Input is the
// surface->light sun tuple with y vertical; the result is NOT normalized
// (retail stores the clamped-negated tuple raw).
std::array<float, 3> slot_projection_direction(
		const std::array<float, 3> &sun_surface_to_light);

// Slot LOD at registration: dynamic silhouette slots take
// bound_radius(16.16) >> 15 + 1 = 2*radius + 1 world units, clamped [6, 20]
// [orig: shadow_decal_alloc_slot @ 0x5d5773..0x5d578a].
int slot_lod_for_radius(float bound_radius_units);

// Blob-only slots (shadow_type 0) take max(width, length) + 7 clamped
// [2, 20] from the authored decal size [orig: @ 0x5d572a..0x5d5767]. No JO
// caller allocates shadow_type 0 (both witnessed callers pass 1 —
// Entity_InitFromModel @ 0x40E1EA, PlayerClass_InitEntity @ 0x4b10f1), so
// this law is ported for completeness of the alloc routine only.
int slot_lod_for_blob(float width_units, float length_units);

// The per-frame grazing rescale of the slot LOD:
// (0.5 + |0.5 / dir_y|) * base_lod, clamped [6, 20]
// [orig: RenderSlot_UpdateEntityLight @ 0x5d6d5c..0x5d6dac].
int grazing_slot_lod(int base_lod, float dir_y);

// Silhouette capture extent: the ortho half-extent is radius * 1.25, clamped
// to radius + 0.75 [orig: RenderSlot_RenderEntityAndChildren
// @ 0x5d783e..0x5d7871 — slot float24/float25].
float silhouette_half_extent(float bound_radius_units);

// Person-type drapes elongate 4x along the projection direction
// [orig: render_sector_model @ 0x5d5d7f..0x5d5d95, itemdef type 3 gate,
// flt_7C44B8 = 4.0].
inline constexpr float kPersonDrapeElongation = 4.0f;

// ---------------------------------------------------------------------------
// Render-target chain and refresh cadence (the retail texture budget)
// ---------------------------------------------------------------------------

// 12 silhouette RTs; base size 256 (512 at shadow detail >= 2, 1024 at
// >= 4), halving after every second slot down to a 32 px floor
// [orig: init_render_target_chain @ 0x5d5320].
inline constexpr int kSlotTextureCount = 12;
int slot_texture_size(int texture_order, int shadow_detail);

// Frame-skip cadence: a slot re-renders when
// (frame & mask) == (slot_index & mask) or its dirty bit is set; mask = 7
// below detail 2, 3 at detail 2, 1 at detail 3, 0 (every frame) at detail 4+
// [orig: RenderSlot_RenderEntityAndChildren @ 0x5d76d9..0x5d76fe]. The
// local player's slot (or its parent vehicle's) skips only below detail 3
// [orig: @ 0x5d7713..0x5d7734].
uint32_t slot_refresh_mask(int shadow_detail);
bool slot_refresh_due(int slot_index, uint32_t frame, uint32_t mask,
		bool dirty);

// ---------------------------------------------------------------------------
// Drape color laws
// ---------------------------------------------------------------------------

// Distance fade of the drape: 0 inside 40 u, (d - 40) / 40 across
// 40..80 u; at >= 80 u the drape is skipped entirely
// [orig: render_sector_model @ 0x5d5d30..0x5d5d53 — 0x280000/0x500000
// fixed thresholds, flt_7DC668 = 1/2621440].
float drape_fade(float camera_distance_units);
bool drape_culled(float camera_distance_units);

// The sun-lit drape ambient: per channel
//   ambient_c = 1 - (1 - fade) * sun_c*|dir_y| / (sun_c*|dir_y| + sky_c)
// with sun = Env_LightBlock, sky = Env_SkyBlock (0..1 here; retail bytes)
// [orig: render_sector_model @ 0x5d5f63..0x5d6008]. The shadow removes only
// the direct sun term scaled by the projection vertical — never the sky
// ambient — which is why a retail noon shadow darkens far more than a
// grazing-clamped dawn shadow.
std::array<float, 3> drape_sun_ambient(const std::array<float, 3> &sun_rgb,
		const std::array<float, 3> &sky_rgb, float dir_y, float fade);

// The same law expressed as the per-channel shadow term q_c =
// sun_c*|dir_y| / (sun_c*|dir_y| + sky_c), so a projective drape shader can
// evaluate ambient = 1 - (1 - fade) * q * silhouette_mask per pixel.
std::array<float, 3> drape_shadow_term(const std::array<float, 3> &sun_rgb,
		const std::array<float, 3> &sky_rgb, float dir_y);

// Attached-light slots (the dominant point light won): the silhouette RT is
// lit by D3D light 4 with NTSC-weighted negated colors
// (c + lum) * 0.5 * -3 (lum = 0.3r + 0.6g + 0.1b) into PS c21..c23
// [orig: RenderSlot_SetupNextLighting @ 0x5d73d3..0x5d740d], and the drape
// scales the light color by -(c + lum) * (1 - fade)
// [orig: render_sector_model @ 0x5d5e89..0x5d5f14, flt_7D4B24 = -2.0
// folded with the 0.5].
std::array<float, 3> slot_light_darkening(const std::array<float, 3> &rgb);
std::array<float, 3> drape_attached_light_scale(
		const std::array<float, 3> &rgb, float fade);

// ---------------------------------------------------------------------------
// Dominant-light pick [orig: RenderSlot_UpdateEntityLight @ 0x5d6a30]
// ---------------------------------------------------------------------------

struct SlotPointLight {
	std::array<float, 3> position{};  // world units
	std::array<float, 3> color{};     // 0..1
	// D3D attenuation form {constant, linear, quadratic, _}
	// (light_scene.h SelectedLight::attenuation).
	std::array<float, 4> attenuation{1.0f, 0.0f, 0.0f, 0.0f};
	uint32_t handle = 0;
};

struct SlotLightPick {
	// light->surface, normalized when a point light wins; the slot default
	// (clamped-negated sun) otherwise.
	std::array<float, 3> direction{};
	uint32_t attached_handle = 0;  // 0 = sun default
};

// Luminance 0.3r + 0.6g + 0.1b over dist^2 * quadratic + constant; the
// strongest passer above the threshold wins (0.1, or 0 for an
// interior-parented entity) and the direction becomes
// normalize(entity - light) [orig: @ 0x5d6b7e..0x5d6c80; threshold
// flt_7C69F4 = 0.1, zeroed @ 0x5d6ae4 when entity+464 is interior-parented].
SlotLightPick pick_dominant_light(const std::array<float, 3> &entity_pos,
		const std::array<float, 3> &default_direction,
		const SlotPointLight *lights, size_t light_count, bool interior);

// ---------------------------------------------------------------------------
// Anchor march [orig: RenderSlot_UpdateEntityLight @ 0x5d6c86..0x5d6d67]
// ---------------------------------------------------------------------------

// Marches from the entity position along the (downward) slot direction in
// unit-planar steps until the terrain height reaches the ray; the vertical
// step is clamped to at least 0.5 u of drop per iteration
// (fixed -32768 [orig: @ 0x5d6cdd..0x5d6cdf]), so for suns below ~30
// degrees the march descends steeper than the true projection — the anchor
// only PLACES the drape patch; the projected UV matrices land the
// silhouette. Coordinates are (x, z planar, y vertical up). Returns the
// planar anchor. march start is the entity position (retail substitutes the
// rotated bbox-center anchor when the entity flag word is zero
// [orig: @ 0x5d6ce7..0x5d6d2d]).
std::array<float, 2> march_shadow_anchor(const std::array<float, 3> &start,
		const std::array<float, 3> &direction,
		const std::function<float(float, float)> &terrain_height,
		int max_steps = 4096);

// ---------------------------------------------------------------------------
// Slot assignment [orig: terrain_sort_and_assign_render_slots @ 0x5d6530]
// ---------------------------------------------------------------------------

inline constexpr int kSlotRecordCount = 256;   // [orig: @ 0x5d56d6]
inline constexpr int kSlotPatchCount = 24;     // drape patch budget
inline constexpr int kSlotCaptureCount = 12;   // silhouette RT budget
// The slot-bind horizon: base score (dist_fixed / 4) past 0x500000 drops
// the record — 320 u [orig: @ 0x5d66b2..0x5d66b4]. (The 80 u cull is the
// separate DRAPE gate — drape_culled above.)
inline constexpr float kSlotBindMaxDistance = 320.0f;

struct SlotCandidateState {
	std::array<float, 2> pos2d{};  // world planar (x, z)
	float bound_radius = 1.0f;     // world units
	bool dead = false;             // entity flag 1 [orig: @ 0x5d6581]
	// Seat-parented (parentSlot 1/2/5, or 3 with a live parent)
	// [orig: @ 0x5d65a0..0x5d65eb].
	bool seat_parented = false;
	// Standing on a vehicle-type ground entity [orig: @ 0x5d661a..0x5d6627].
	bool on_vehicle = false;
	bool is_local_player_or_parent = false;  // halves the score [orig: @ 0x5d6864]
	bool interior = false;
	bool dynamic = true;           // silhouette-class (person / DynamicShadow)
	bool has_blob_texture = false; // authored items.def `shadow` decal
	bool is_person = false;        // 4x drape elongation class
};

struct SlotAssignment {
	uint64_t id = 0;
	// Stable table index — the retail refresh cadence keys on it
	// [orig: RenderSlot_RenderEntityAndChildren @ 0x5d771d, handle & mask].
	int record_index = -1;
	bool bound = false;            // holds one of the 24 drape patches
	int capture_order = -1;        // 0..11 silhouette RT index, -1 = none
	bool capture_dirty = false;    // RT index changed this frame
	// Drape classification [orig: RenderSlot_DrawAllDrapes @ 0x5d6e54..]:
	// a bound dynamic slot with an RT drapes its silhouette; a bound slot
	// without one drapes the authored blob (when the item authors one).
	bool draws_silhouette = false;
	bool draws_blob = false;
	// Excluded from its own slot this frame (seat/vehicle/dead) — the
	// silhouette-render leg re-checks the same predicates
	// [orig: RenderSlot_RenderEntityAndChildren @ 0x5d774e..0x5d77b3].
	bool excluded = false;
};

// Persistent slot bindings — retail keeps patch and RT indices sticky across
// frames and only marks a capture dirty when its order changes
// [orig: @ 0x5d69c8..0x5d69d9].
class RenderSlotPlan {
public:
	// Registration mirrors shadow_decal_alloc_slot: idempotent per id, fails
	// past 256 records [orig: @ 0x5d5690].
	bool register_entity(uint64_t id);
	void release_entity(uint64_t id);
	size_t registered_count() const { return records_.size(); }

	// Scores, sorts, and (re)binds every registered record for this frame.
	// view_dir2d is the planar camera forward (need not be normalized; a
	// zero vector drops the view-alignment weighting exactly like retail's
	// degenerate-length branch [orig: @ 0x5d671b..0x5d6767]).
	// The state callback supplies the per-entity inputs.
	std::vector<SlotAssignment> assign(
			const std::array<float, 2> &camera_pos2d,
			const std::array<float, 2> &view_dir2d,
			const std::function<SlotCandidateState(uint64_t)> &state_for);

private:
	struct Record {
		uint64_t id = 0;
		bool bound = false;
		int patch_index = -1;
		int capture_order = -1;
	};
	std::vector<Record> records_;
	std::array<bool, kSlotPatchCount> patch_used_{};
};

// The retail priority score for one candidate: 2D camera distance / 4,
// weighted by view alignment (1.5 - dot terms, Q16 fixed math), halved for
// the local player or its parent; excluded candidates score
// kSlotScoreExcluded [orig: @ 0x5d6535..0x5d6871]. Exposed for tests.
inline constexpr int32_t kSlotScoreExcluded = 0x40000000;
int32_t slot_priority_score(const std::array<float, 2> &camera_pos2d,
		const std::array<float, 2> &view_dir2d,
		const SlotCandidateState &state);

}  // namespace renderer
