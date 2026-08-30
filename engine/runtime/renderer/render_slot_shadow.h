// The render-slot entity ground-shadow planner — the portable half of the
// retail per-entity shadow pipeline (docs/render/render-lighting-re.md,
// "the render-slot (entity ground shadow) side").
//
// Retail shape: a 256-record slot table (128 B each)
// [orig: RenderSlot_Table @ 0x2be3d30] registered at entity init — persons
// always, items via the attrib2 DynamicShadow bit, gated on the shadow-detail
// option [orig: Entity_InitFromModel @ 0x40E1C8..0x40E1F7 ->
// RenderSlot_AllocSlot @ 0x5d5690]. Each frame the slot pass
// [orig: render_shadow_pass @ 0x5d7b70]:
//   1. loads the sun into the slot default direction, clamps the vertical
//      component to 0.25 and negates all three (light->surface form; a
//      grazing sun never stretches a silhouette past 4x height);
//   2. scores and sorts every slot by camera distance x view alignment,
//      binds the best 24 to drape patches and the first 12 to silhouette
//      render targets [orig: RenderSlot_SortAndAssign
//      @ 0x5d6530; RT chain RenderSlot_InitTextureChain @ 0x5d5320];
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
//      [orig: RenderSlot_DrawAllDrapes @ 0x5d6e20 -> RenderSlot_DrawSilhouetteDrape
//      (drape) @ 0x5d5ca0 / RenderSlot_DrawAuthoredBlobDecal (authored blob)
//      @ 0x5d59d0].
//
// This unit carries every planning/selection/color law as a structural
// translation; the device half (godot/src) realizes the silhouette capture
// and the terrain drape. The retail anchor march PLACES the lod x lod
// terrain patch the drape is drawn over (slot_patch_bounds), and the drape's
// second texture stage clips the projected silhouette by depth
// (slot_depth_clip over shadowztex_pixels) — a projective device computes the
// projection per pixel but must still bound it by that patch and that clip,
// or every object's shadow runs the whole capture frustum (the 2026-08-22
// "objects cast too-tall shadows" report).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace opennova::renderer {

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
// [orig: RenderSlot_AllocSlot @ 0x5d5773..0x5d578a].
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

// The silhouette capture view. Retail builds a rotation-only D3D view from
// the slot direction [orig: setup_shadow_cascade_matrices @ 0x58d300 ->
// build_direction_look_at_matrix @ 0x612c90]: forward = normalize(dir),
// right = normalize(forward.z, 0, -forward.x), up = forward x right (a
// vertical direction, |forward.xz| = 0, leaves right and up ZERO — retail's
// degenerate zenith matrix; the device substitutes the world x axis so the
// capture keeps a frame). The entity renders at the origin of that view
// [orig: Entity_RenderWithLODCallback @ 0x5d6ef0 zeroes the position for a
// null origin; children at their offset from the parent @ 0x5d795a/0x5d79c8]
// under an orthographic projection of scale 1/half_extent over the depth
// band 0.2..5000.2 [orig: @ 0x58d38b..0x58d3a3 — 1/(far - near) = 0.0002,
// -near/(far - near) = -0.00004].
struct SlotCaptureBasis {
	std::array<float, 3> right{};
	std::array<float, 3> up{};
	std::array<float, 3> forward{};
	bool degenerate = false;  // retail's zero right/up (vertical direction)
};
SlotCaptureBasis silhouette_capture_basis(const std::array<float, 3> &direction);
inline constexpr float kSilhouetteCaptureNear = 0.2f;     // @ 0x58d3a3
inline constexpr float kSilhouetteCaptureFar = 5000.2f;   // @ 0x58d399
// Device fold: a Godot ortho camera clips outside [near, far] like retail's
// D3D band, so the capture eye backs off from the caster center along
// -forward far enough that the whole model sphere lies inside its band —
// the eye distance and the band below stand in for retail's origin eye
// and 0.2..5000.2 band; the projected silhouette is identical.
struct SlotCaptureEye {
	float distance = 0.0f;  // eye = center - forward * distance
	float near = 0.0f;
	float far = 0.0f;
};
SlotCaptureEye silhouette_capture_eye(float model_sphere_radius_units);

// ---------------------------------------------------------------------------
// Render-target chain and refresh cadence (the retail texture budget)
// ---------------------------------------------------------------------------

// 12 silhouette RTs; base size 256 (512 at shadow detail >= 2, 1024 at
// >= 4), halving after every second slot down to a 32 px floor
// [orig: RenderSlot_InitTextureChain @ 0x5d5320].
inline constexpr int kSlotTextureCount = 12;
int slot_texture_size(int texture_order, int shadow_detail);
// Each capture clears its RT to 0x00FFFFFF — white RGB, alpha 0 — before
// the PROJSHAD black draws [orig: RenderSlot_RenderEntityAndChildren
// @ 0x5d780f, GTexRT_SelectThunk with color_mask 0xFFFFFF]; the drape reads
// the RGB, so a white texel is the no-shadow sample.
inline constexpr uint32_t kSlotCaptureClearArgb = 0x00FFFFFFu;

// Frame-skip cadence: a slot re-renders when
// (frame & mask) == (slot_index & mask) or its dirty bit is set; mask = 7
// below detail 2, 3 at detail 2, 1 at detail 3, 0 (every frame) at detail 4+
// [orig: RenderSlot_RenderEntityAndChildren @ 0x5d76d9..0x5d76fe]. The
// local player's slot (or its parent vehicle's) skips only below detail 3
// [orig: @ 0x5d7713..0x5d7734].
uint32_t slot_refresh_mask(int shadow_detail);
// The mask one slot refreshes on: the local player's slot (or its parent
// vehicle's) refreshes every frame from detail 3 up
// [orig: @ 0x5d7713..0x5d7734].
uint32_t slot_refresh_mask_for(int shadow_detail,
		bool is_local_player_or_parent);
bool slot_refresh_due(int slot_index, uint32_t frame, uint32_t mask,
		bool dirty);

// The local player's first-person drape gate: its own drape is skipped while
// prone-latched or below shadow detail 2
// [orig: RenderSlot_DrawAllDrapes @ 0x5d6e70..0x5d6e90].
bool local_first_person_drape_skipped(bool first_person, bool prone,
		int shadow_detail);

// ---------------------------------------------------------------------------
// Drape color laws
// ---------------------------------------------------------------------------

// Distance fade of the drape: 0 inside 40 u, (d - 40) / 40 across
// 40..80 u; at >= 80 u the drape is skipped entirely
// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5d30..0x5d5d53 — 0x280000/0x500000
// fixed thresholds, flt_7DC668 = 1/2621440]. The device drape shader takes
// the pair as a uniform from these constants.
inline constexpr float kDrapeFadeStartUnits = 40.0f;  // 0x280000
inline constexpr float kDrapeFadeEndUnits = 80.0f;    // 0x500000
float drape_fade(float camera_distance_units);
bool drape_culled(float camera_distance_units);

// The sun-lit drape ambient: per channel
//   ambient_c = 1 - (1 - fade) * sun_c*|dir_y| / (sun_c*|dir_y| + sky_c)
// with sun = Env_LightBlock, sky = Env_SkyBlock (0..1 here; retail bytes)
// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5f63..0x5d6008]. The shadow removes only
// the direct sun term scaled by the projection vertical — never the sky
// ambient — which is why a retail noon shadow darkens far more than a
// grazing-clamped dawn shadow.
std::array<float, 3> drape_sun_ambient(const std::array<float, 3> &sun_rgb,
		const std::array<float, 3> &sky_rgb, float dir_y, float fade);

// The same law expressed as the per-channel shadow term q_c =
// sun_c*|dir_y| / (sun_c*|dir_y| + sky_c), so a projective drape shader can
// evaluate the fixed-function material ambient.
std::array<float, 3> drape_shadow_term(const std::array<float, 3> &sun_rgb,
		const std::array<float, 3> &sky_rgb, float dir_y);

// The two fixed-function texture stages and framebuffer multiply used by a
// silhouette drape. The slot RT is cleared white and the PROJSHAD pass draws
// black, preserving its resolved/filter edge as RGB. Stage 0 ADDs that sample
// to diffuse ambient = 1 - (1 - fade) * q; stage 1 ADDs shadowztex; each ADD
// saturates [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5ca0]. A white RT
// sample or depth-clip sample therefore suppresses the shadow.
std::array<float, 3> drape_silhouette_factor(
		const std::array<float, 3> &capture_rgb,
		const std::array<float, 3> &shadow_term, float fade,
		float depth_clip);

// Attached-light slots (the dominant point light won): the silhouette RT is
// lit by D3D light 4 with NTSC-weighted negated colors
// (c + lum) * 0.5 * -3 (lum = 0.3r + 0.6g + 0.1b) into PS c21..c23
// [orig: RenderSlot_SetupNextLighting @ 0x5d73d3..0x5d740d], and the drape
// scales the light color by -(c + lum) * (1 - fade)
// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5e89..0x5d5f14, flt_7D4B24 = -2.0
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

// A caller-side contact reconciliation before the exact retail march. Retail
// reads the live simulated entity position, while a presentation-only mission
// caster can remain at its authored pose a handful of Q16 ticks above the
// raw16 terrain surface. A gap no larger than one terrain-height quantum is
// ground contact at the resolution of that height query; keeping it would
// turn the tiny vertical gap into one whole planar march step.
inline constexpr float kSlotTerrainHeightQuantumUnits = 1.0f / 256.0f;
float slot_march_start_height(float caster_height, float terrain_height,
		float contact_tolerance);

// Marches from the entity position along the (downward) slot direction in
// unit-planar steps until the terrain height reaches the ray; the vertical
// step keeps the direction's own rate and is SUBSTITUTED by 0.5 u of drop
// only when it would not descend (fixed -32768 stored for a non-negative
// step [orig: @ 0x5d6cd7..0x5d6cdf]). The anchor PLACES the drape patch
// (slot_patch_bounds); the projected UV matrices land the silhouette.
// Coordinates are (x, z planar, y vertical up). Returns the planar anchor.
// march start is the entity position (retail substitutes the rotated
// bbox-center anchor when the entity flag word is zero
// [orig: @ 0x5d6ce7..0x5d6d2d]).
std::array<float, 2> march_shadow_anchor(const std::array<float, 3> &start,
		const std::array<float, 3> &direction,
		const std::function<float(float, float)> &terrain_height,
		int max_steps = 4096);

// ---------------------------------------------------------------------------
// The drape patch [orig: RenderSlot_RebuildPatchVertexBuffer @ 0x5d5130]
// ---------------------------------------------------------------------------

// The lod x lod world-axis-aligned square the drape is drawn over: its
// origin is the marched anchor backed off lod/2 west and lod/2 north,
// rounded to the lod band's grid (1 u below lod 10, 2 u for 10..15, 4 u from
// 16), and the (lod + 1)^2 terrain-following vertices run east and south at
// 1 u — so no entity's ground shadow ever covers more than the 20 u cap
// [orig: @ 0x5d5142..0x5d519d the origin and its snap; @ 0x5d527a/@ 0x5d52f7
//  the column/row steps]. Planar mission units (x east, north).
struct SlotPatch {
	float min_x = 0.0f;
	float max_x = 0.0f;
	float min_north = 0.0f;
	float max_north = 0.0f;
};
SlotPatch slot_patch_bounds(float anchor_x, float anchor_north, int lod);

// ---------------------------------------------------------------------------
// The depth-clip stage [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5ca0 ->
// build_shadow_cascade_uv_matrices @ 0x58cf10; shadow_system_init_resources
// @ 0x5d6230]
// ---------------------------------------------------------------------------

// "shadowztex": the 32x4 ARGB texture the drape's second stage ADDs to the
// silhouette term — rows 0..2 are white below column 16, one gray texel
// at column 16 and black beyond; row 3 is all white. Sampled CLAMP +
// bilinear [orig: @ 0x5d6260..0x5d62a7 the fill; flags 1 -> clamp
//  (apply_texture_stages @ 0x680870), linear min/mag]. Because the stage
// ADDs and saturates, WHITE suppresses the shadow and BLACK keeps it:
// u2 > 0.5 (ground beyond the plane through the caster, away from the
// light) draws, u2 < 0.5 (toward the light) is suppressed, and v2 >= 0.75
// (far along the light) fades to white.
inline constexpr int kShadowZTexWidth = 32;
inline constexpr int kShadowZTexHeight = 4;
std::array<uint32_t, kShadowZTexWidth * kShadowZTexHeight> shadowztex_pixels();

// The stage's texgen, as world-space dot products: u2 = 0.5 + k·f2·(p − lp),
// v2 = 0.5 + 0.333·k²·f1·(p − lp) with k = 0.5 / half_size, lp = entity
// position − the slot direction, f1 = the normalized slot direction and f2
// the same direction with its VERTICAL component x 4 for a person-class
// caster (itemdef type 3) before normalizing — the steepened clip plane
// that cuts a soldier's shadow nearer its feet. u2 = u_axis·p + u_offset,
// v2 = v_axis·p + v_offset. [orig: the two direction copies and the x4
//  @ 0x5d5d66..0x5d5d91; light_pos @ 0x5d5d95..0x5d5dd4; k @ 0x58cf2f; the
//  detail u row @ 0x58d1cc..0x58d204; the detail v row from the k-scaled
//  depth column x 0.333 k @ 0x58d222..0x58d249]
inline constexpr float kPersonClipSteepening = 4.0f;  // flt_7C44B8
struct SlotDepthClip {
	std::array<float, 3> u_axis{};
	float u_offset = 0.5f;
	std::array<float, 3> v_axis{};
	float v_offset = 0.5f;
};
SlotDepthClip slot_depth_clip(const std::array<float, 3> &slot_direction,
		float half_size, bool person, const std::array<float, 3> &entity_pos);

// ---------------------------------------------------------------------------
// Slot assignment [orig: RenderSlot_SortAndAssign @ 0x5d6530]
// ---------------------------------------------------------------------------

inline constexpr int kSlotRecordCount = 256;   // [orig: @ 0x5d56d6]
inline constexpr int kSlotPatchCount = 24;     // drape patch budget
inline constexpr int kSlotCaptureCount = 12;   // silhouette RT budget
// The slot-bind horizon: base score (dist_fixed / 4) past 0x500000 drops
// the record — 320 u [orig: @ 0x5d66b2..0x5d66b4]. (The 80 u cull is the
// separate DRAPE gate — drape_culled above.)
inline constexpr float kSlotBindMaxDistance = 320.0f;

// The is_person class (itemdef +0x5C == 3) does NOT stretch the drape. The
// 4.0 (flt_7C44B8) multiplies the VERTICAL component of a COPY of the slot
// direction, and that copy builds only the drape's SECOND texture matrix —
// the depth-clip stage: "shadowztex" (shadow_system_init_resources
// @ 0x5d62d2), a 32x4 white/black step addressed by depth along that
// steepened direction, which clips the projected silhouette to the
// half-space beyond the caster and, for a soldier, nearer its feet. The
// silhouette projection itself uses the unscaled direction
// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5d7f..0x5d5de1 ->
// build_shadow_cascade_uv_matrices @ 0x58cf10: lookat_dir1 (primary,
// unscaled) vs lookat_dir2 (detail, y x4); re-witnessed 2026-08-21 — the
// earlier "elongate 4x along the direction" reading drew every person
// shadow four times its projected length]. The depth-clip stage is
// slot_depth_clip above (ported 2026-08-22).
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
	bool is_person = false;        // itemdef type 3: the depth-clip stage's steepened class
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
	// Registration mirrors RenderSlot_AllocSlot: idempotent per id, fails
	// past 256 records [orig: @ 0x5d5690].
	bool register_entity(uint64_t id);
	void release_entity(uint64_t id);
	size_t registered_count() const { return live_count_; }

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
		bool live = false;
		bool bound = false;
		int patch_index = -1;
		int capture_order = -1;
	};
	// The fixed 256-record table: an entity keeps its index for its
	// lifetime, so the refresh cadence keyed on it never re-phases when
	// another record is released [orig: RenderSlot_AllocSlot @ 0x5d5690
	// appends at RenderSlot_Count into RenderSlot_Table @ 0x2be3d30, and the
	// count only resets at subsystem init @ 0x5d61cb — retail binds a slot
	// per entity for the mission and never releases]. Device fold: a Godot
	// caster is an instance id that a respawn recreates, so release_entity
	// exists and the lowest free index is reused to keep the table bounded;
	// a live record's index is as stable as retail's.
	std::array<Record, kSlotRecordCount> records_{};
	size_t live_count_ = 0;
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

}  // namespace opennova::renderer